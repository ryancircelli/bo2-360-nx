// XamContent* guest entry points on a real kernel state: create/open/close/delete/enumerate with the
// CreateDisposition flags (1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING ...), the delete-then-create
// path NFSMW uses, a package mounted for the whole session (Forza), title id resolution, overlapped results.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>

namespace rex::kernel::xam {
u32 XamContentCreateEx_entry(u32, mapped_string, mapped_void, u32, mapped_u32, mapped_u32, u32, u64,
                             mapped_void);
u32 XamContentCreate_entry(u32, mapped_string, mapped_void, u32, mapped_u32, mapped_u32, mapped_void);
u32 XamContentClose_entry(mapped_string, mapped_void);
u32 XamContentFlush_entry(mapped_string, mapped_void);
u32 XamContentDelete_entry(u32, mapped_void, mapped_void);
u32 XamContentGetCreator_entry(u32, mapped_void, mapped_u32, mapped_u64, mapped_void);
u32 XamContentGetThumbnail_entry(u32, mapped_void, mapped_void, mapped_u32, mapped_void);
u32 XamContentSetThumbnail_entry(u32, mapped_void, mapped_void, u32, mapped_void);
u32 XamContentCreateEnumerator_entry(u32, u32, u32, u32, u32, mapped_u32, mapped_u32);
}  // namespace rex::kernel::xam

using namespace rex::kernel::xam;
using rex::X_STATUS;
using rex::system::XContentType;
using rex::system::xam::XCONTENT_DATA;

namespace {

struct Fixture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("rex-xamcontent-" + std::to_string(std::chrono::steady_clock::now()
                                                                       .time_since_epoch()
                                                                       .count()));
  std::unique_ptr<rex::Runtime> runtime;
  Fixture() {
    std::filesystem::create_directories(path);
    REQUIRE(rex::cvar::SetFlagFromCommandLine("content_backup_root", ""));
    runtime = std::make_unique<rex::Runtime>(path);
    rex::RuntimeConfig config{};
    config.tool_mode = true;
    REQUIRE(runtime->Setup(std::move(config)) == X_STATUS_SUCCESS);
  }
  ~Fixture() {
    runtime.reset();
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  rex::system::KernelState* kernel() { return runtime->kernel_state(); }
  rex::system::xam::ContentManager* manager() { return kernel()->content_manager(); }
};

XCONTENT_DATA MakeData(const char* file, XContentType type = XContentType::kSavedGame) {
  XCONTENT_DATA d{};
  d.device_id = 1;
  d.content_type = type;
  d.set_file_name(file);
  d.set_display_name(u"Test");
  return d;
}

// What the guest passes: all host buffers (guest address 0) except where an overlapped needs real guest memory.
struct Call {
  XCONTENT_DATA data;
  rex::be<uint32_t> disposition;
  rex::be<uint32_t> license;
  char root[16];
  Call(const char* root_name, const XCONTENT_DATA& d) : data(d), disposition(0xAAAAAAAA), license(0x55555555) {
    std::strncpy(root, root_name, sizeof(root) - 1);
    root[sizeof(root) - 1] = 0;
  }
  uint32_t Create(uint32_t mode, rex::mapped_void overlapped = nullptr) {
    return XamContentCreateEx_entry(0, rex::mapped_string(root, 0), rex::mapped_void(&data, 0), mode,
                                    rex::mapped_u32(&disposition, 0), rex::mapped_u32(&license, 0), 0, 0,
                                    overlapped);
  }
  uint32_t Close() { return XamContentClose_entry(rex::mapped_string(root, 0), nullptr); }
  uint32_t Delete() { return XamContentDelete_entry(0, rex::mapped_void(&data, 0), nullptr); }
};

constexpr uint32_t kDispUnknown = 0, kDispCreate = 1, kDispOpen = 2;

}  // namespace

TEST_CASE("XamContentCreate mode 3 opens only what exists and mode 1 refuses what exists", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  CHECK(c.Create(3) == X_ERROR_PATH_NOT_FOUND);  // OPEN_EXISTING of nothing
  CHECK(c.disposition == kDispUnknown);
  REQUIRE(c.Create(1) == X_ERROR_SUCCESS);  // CREATE_NEW
  CHECK(c.disposition == kDispCreate);
  REQUIRE(c.Close() == X_ERROR_SUCCESS);
  CHECK(c.Create(1) == X_ERROR_ALREADY_EXISTS);  // CREATE_NEW over an existing package
  CHECK(c.disposition == kDispUnknown);
  REQUIRE(c.Create(3) == X_ERROR_SUCCESS);  // OPEN_EXISTING of the one made above
  CHECK(c.disposition == kDispOpen);
  CHECK(c.Close() == X_ERROR_SUCCESS);
  CHECK(c.Close() == X_ERROR_FILE_NOT_FOUND);  // closing twice
}

TEST_CASE("XamContentCreate mode 2 deletes then recreates: the NFSMW save path", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("NFSMW"));
  REQUIRE(c.Create(2) == X_ERROR_SUCCESS);  // CREATE_ALWAYS with nothing there
  CHECK(c.disposition == kDispCreate);
  auto dir = f.manager()->GetOpenPackagePath("SAVE");
  REQUIRE_FALSE(dir.empty());
  std::ofstream(dir / "old.dat") << "stale";
  REQUIRE(c.Close() == X_ERROR_SUCCESS);
  // Second save: same name, mode 2. The old contents must be gone and the game must be told "created".
  REQUIRE(c.Create(2) == X_ERROR_SUCCESS);
  CHECK(c.disposition == kDispCreate);
  dir = f.manager()->GetOpenPackagePath("SAVE");
  CHECK_FALSE(std::filesystem::exists(dir / "old.dat"));
  CHECK(c.Close() == X_ERROR_SUCCESS);
}

TEST_CASE("XamContentCreate mode 2 with the root still mounted unmounts it first", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  REQUIRE(c.Create(2) == X_ERROR_SUCCESS);
  std::ofstream(f.manager()->GetOpenPackagePath("SAVE") / "a.dat") << "x";
  // The game never closed: reuses the same root name.
  REQUIRE(c.Create(2) == X_ERROR_SUCCESS);
  CHECK(c.disposition == kDispCreate);
  CHECK_FALSE(std::filesystem::exists(f.manager()->GetOpenPackagePath("SAVE") / "a.dat"));
  CHECK(c.Close() == X_ERROR_SUCCESS);
}

TEST_CASE("XamContentCreate mode 4 creates then reopens and reports the license mask", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  REQUIRE(c.Create(4) == X_ERROR_SUCCESS);  // OPEN_ALWAYS
  CHECK(c.disposition == kDispCreate);
  REQUIRE(c.Close() == X_ERROR_SUCCESS);
  c.license = 0x55555555;
  REQUIRE(c.Create(4) == X_ERROR_SUCCESS);
  CHECK(c.disposition == kDispOpen);
  CHECK(c.license == 0);  // CreateContent wrote a header without a license word
  CHECK(c.Close() == X_ERROR_SUCCESS);
}

TEST_CASE("XamContentCreate mode 5 truncates only what exists", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  CHECK(c.Create(5) == X_ERROR_PATH_NOT_FOUND);  // TRUNCATE_EXISTING of nothing
  REQUIRE(c.Create(1) == X_ERROR_SUCCESS);
  std::ofstream(f.manager()->GetOpenPackagePath("SAVE") / "keep.dat") << "x";
  REQUIRE(c.Close() == X_ERROR_SUCCESS);
  REQUIRE(c.Create(5) == X_ERROR_SUCCESS);
  CHECK(c.disposition == kDispCreate);
  CHECK_FALSE(std::filesystem::exists(f.manager()->GetOpenPackagePath("SAVE") / "keep.dat"));
  CHECK(c.Close() == X_ERROR_SUCCESS);
}

TEST_CASE("XamContentCreate rejects an unknown mode and a bad data size", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  CHECK(c.Create(9) == X_ERROR_INVALID_PARAMETER);
  CHECK(c.disposition == kDispUnknown);
  CHECK_FALSE(f.manager()->ContentExists(f.kernel()->user_profile(0)->xuid(), MakeData("slot1")));
}

TEST_CASE("A package mounted for the whole session blocks a second mount and survives other roots", "[runtime][content][xam]") {
  // Forza opens its save once and keeps it for the whole session.
  Fixture f;
  Call a("SAVE", MakeData("profile"));
  REQUIRE(a.Create(4) == X_ERROR_SUCCESS);
  std::ofstream(f.manager()->GetOpenPackagePath("SAVE") / "p.dat") << "session";
  // Same root, mode 4: the manager refuses the second mount instead of dropping the first.
  Call again("SAVE", MakeData("profile"));
  CHECK(again.Create(4) == X_ERROR_ALREADY_EXISTS);
  CHECK(f.manager()->GetOpenPackagePath("SAVE").filename() == "profile");
  // Another root for the same package is allowed and sees the same files.
  Call other("OTHER", MakeData("profile"));
  REQUIRE(other.Create(3) == X_ERROR_SUCCESS);
  CHECK(std::filesystem::exists(f.manager()->GetOpenPackagePath("OTHER") / "p.dat"));
  // A mounted package cannot be deleted from under the game.
  CHECK(a.Delete() == X_ERROR_ACCESS_DENIED);
  CHECK(other.Close() == X_ERROR_SUCCESS);
  CHECK(a.Delete() == X_ERROR_ACCESS_DENIED);
  CHECK(a.Close() == X_ERROR_SUCCESS);
  CHECK(a.Delete() == X_ERROR_SUCCESS);
  CHECK(a.Delete() == X_ERROR_FILE_NOT_FOUND);
}

TEST_CASE("Title id resolution: the running title and an explicit id are the same package only when equal", "[runtime][content][xam]") {
  Fixture f;
  const uint64_t xuid = f.kernel()->user_profile(0)->xuid();
  auto running = rex::system::xam::XCONTENT_AGGREGATE_DATA(MakeData("slot1"));  // title = kCurrentlyRunningTitleId
  REQUIRE(f.manager()->CreateContent("a", xuid, running) == X_ERROR_SUCCESS);
  REQUIRE(f.manager()->WriteContentHeaderFile(xuid, running) == X_ERROR_SUCCESS);
  auto explicit_same = running;
  explicit_same.title_id = f.kernel()->title_id();
  auto explicit_other = running;
  explicit_other.title_id = f.kernel()->title_id() ^ 0x1u;
  CHECK(f.manager()->ContentExists(xuid, explicit_same));
  CHECK_FALSE(f.manager()->ContentExists(xuid, explicit_other));
  CHECK(f.manager()->IsContentOpen(xuid, explicit_same));
  CHECK_FALSE(f.manager()->IsContentOpen(xuid, explicit_other));
  // Listing by the sentinel and by the real id agree, the other title has nothing.
  const auto t = XContentType::kSavedGame;
  CHECK(f.manager()->ListContent(1, xuid, t, rex::system::xam::kCurrentlyRunningTitleId).size() == 1);
  CHECK(f.manager()->ListContent(1, xuid, t, f.kernel()->title_id()).size() == 1);
  CHECK(f.manager()->ListContent(1, xuid, t, f.kernel()->title_id() ^ 0x1u).empty());
  CHECK(f.manager()->CloseContent("a") == X_ERROR_SUCCESS);
}

TEST_CASE("Marketplace content belongs to xuid 0 whoever asks", "[runtime][content][xam]") {
  Fixture f;
  auto data = MakeData("dlc1", XContentType::kMarketplaceContent);
  rex::system::xam::XCONTENT_AGGREGATE_DATA agg(data);
  const uint64_t a = 0xE000000000000001ull, b = 0xE000000000000002ull;
  REQUIRE(f.manager()->CreateContent("dlc", a, agg) == X_ERROR_SUCCESS);
  REQUIRE(f.manager()->CloseContent("dlc") == X_ERROR_SUCCESS);
  CHECK(f.manager()->ContentExists(a, agg));
  CHECK(f.manager()->ContentExists(b, agg));
  CHECK(f.manager()->ContentExists(0, agg));
}

TEST_CASE("Content enumeration lists user and common content with the host path", "[runtime][content][xam]") {
  Fixture f;
  Call a("S1", MakeData("slot1"));
  Call b("S2", MakeData("slot2"));
  REQUIRE(a.Create(1) == X_ERROR_SUCCESS);
  REQUIRE(b.Create(1) == X_ERROR_SUCCESS);
  REQUIRE(a.Close() == X_ERROR_SUCCESS);
  REQUIRE(b.Close() == X_ERROR_SUCCESS);
  rex::be<uint32_t> size = 0, handle = 0;
  REQUIRE(XamContentCreateEnumerator_entry(0, 1 /*HDD*/, uint32_t(XContentType::kSavedGame), 0, 8,
                                           rex::mapped_u32(&size, 0), rex::mapped_u32(&handle, 0)) ==
          X_ERROR_SUCCESS);
  CHECK(uint32_t(size) == sizeof(XCONTENT_DATA) * 8);
  REQUIRE(uint32_t(handle) != 0);
  auto e = f.kernel()->object_table()->LookupObject<rex::system::XEnumerator>(handle);
  REQUIRE(e);
  CHECK(e->item_count() == 2);
  // Unknown device: invalid argument and the size is zeroed.
  size = 77;
  CHECK(XamContentCreateEnumerator_entry(0, 99, 1, 0, 8, rex::mapped_u32(&size, 0),
                                         rex::mapped_u32(&handle, 0)) == X_E_INVALIDARG);
  CHECK(uint32_t(size) == 0);
  // The optical drive has no content.
  rex::be<uint32_t> h2 = 0;
  REQUIRE(XamContentCreateEnumerator_entry(0, 2 /*ODD*/, 1, 0, 8, rex::mapped_u32(&size, 0),
                                           rex::mapped_u32(&h2, 0)) == X_ERROR_SUCCESS);
  CHECK(f.kernel()->object_table()->LookupObject<rex::system::XEnumerator>(h2)->item_count() == 0);
  // No such user.
  CHECK(XamContentCreateEnumerator_entry(3, 1, 1, 0, 8, rex::mapped_u32(&size, 0), rex::mapped_u32(&h2, 0)) ==
        X_ERROR_NO_SUCH_USER);
}

TEST_CASE("Content calls with an invalid user fail without touching the disk", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  const uint32_t r = XamContentCreateEx_entry(3, rex::mapped_string(c.root, 0), rex::mapped_void(&c.data, 0), 1,
                                              rex::mapped_u32(&c.disposition, 0), rex::mapped_u32(&c.license, 0), 0, 0,
                                              nullptr);
  CHECK(r == X_ERROR_NO_SUCH_USER);
  CHECK(f.manager()->GetOpenPackagePath("SAVE").empty());
}

TEST_CASE("Thumbnails round trip and a missing one reports not found with size 0", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  REQUIRE(c.Create(1) == X_ERROR_SUCCESS);
  REQUIRE(c.Close() == X_ERROR_SUCCESS);
  rex::be<uint32_t> size = 100;
  uint8_t buf[8] = {};
  CHECK(XamContentGetThumbnail_entry(0, rex::mapped_void(&c.data, 0), rex::mapped_void(buf, 0),
                                     rex::mapped_u32(&size, 0), nullptr) == X_ERROR_FILE_NOT_FOUND);
  CHECK(uint32_t(size) == 0);
  const uint8_t png[4] = {0x89, 'P', 'N', 'G'};
  REQUIRE(XamContentSetThumbnail_entry(0, rex::mapped_void(&c.data, 0), rex::mapped_void(const_cast<uint8_t*>(png), 0),
                                       4, nullptr) == X_ERROR_SUCCESS);
  size = 2;  // too small: reports the needed size and an error
  CHECK(XamContentGetThumbnail_entry(0, rex::mapped_void(&c.data, 0), rex::mapped_void(buf, 0),
                                     rex::mapped_u32(&size, 0), nullptr) == X_ERROR_INSUFFICIENT_BUFFER);
  CHECK(uint32_t(size) == 4);
  size = 8;
  REQUIRE(XamContentGetThumbnail_entry(0, rex::mapped_void(&c.data, 0), rex::mapped_void(buf, 0),
                                       rex::mapped_u32(&size, 0), nullptr) == X_ERROR_SUCCESS);
  CHECK(std::memcmp(buf, png, 4) == 0);
}

TEST_CASE("GetCreator: saves are the user's, other content has no creator, missing is not found", "[runtime][content][xam]") {
  Fixture f;
  Call c("SAVE", MakeData("slot1"));
  rex::be<uint32_t> is_creator = 5;
  rex::be<uint64_t> creator = 5;
  CHECK(XamContentGetCreator_entry(0, rex::mapped_void(&c.data, 0), rex::mapped_u32(&is_creator, 0),
                                   rex::mapped_u64(&creator, 0), nullptr) == X_ERROR_PATH_NOT_FOUND);
  REQUIRE(c.Create(1) == X_ERROR_SUCCESS);
  REQUIRE(c.Close() == X_ERROR_SUCCESS);
  REQUIRE(XamContentGetCreator_entry(0, rex::mapped_void(&c.data, 0), rex::mapped_u32(&is_creator, 0),
                                     rex::mapped_u64(&creator, 0), nullptr) == X_ERROR_SUCCESS);
  CHECK(uint32_t(is_creator) == 1);
  CHECK(uint64_t(creator) == f.kernel()->user_profile(0)->xuid());
}

TEST_CASE("Overlapped content calls complete through the overlapped block", "[runtime][content][xam][overlapped]") {
  Fixture f;
  auto* mem = f.kernel()->memory();
  const uint32_t guest = mem->SystemHeapAlloc(0x40);
  REQUIRE(guest != 0);
  void* host = mem->TranslateVirtual(guest);
  std::memset(host, 0, 0x40);
  rex::mapped_void overlapped(host, guest);

  // Close is completed immediately: IO_PENDING returned, the result is already in the block.
  Call c("SAVE", MakeData("slot1"));
  REQUIRE(c.Create(1) == X_ERROR_SUCCESS);
  CHECK(XamContentClose_entry(rex::mapped_string(c.root, 0), overlapped) == X_ERROR_IO_PENDING);
  CHECK(rex::system::XOverlappedGetResult(host) == X_ERROR_SUCCESS);
  // Closing something not open completes with the error, length -1 (some games read length as failure).
  CHECK(XamContentClose_entry(rex::mapped_string(c.root, 0), overlapped) == X_ERROR_IO_PENDING);
  CHECK(rex::system::XOverlappedGetResult(host) == X_ERROR_FILE_NOT_FOUND);
  CHECK(rex::system::XOverlappedGetLength(host) == 0xFFFFFFFFu);
  // Flush is always success.
  CHECK(XamContentFlush_entry(rex::mapped_string(c.root, 0), overlapped) == X_ERROR_IO_PENDING);
  CHECK(rex::system::XOverlappedGetResult(host) == X_ERROR_SUCCESS);
  // Invalid user completes immediately with the error.
  CHECK(XamContentDelete_entry(3, rex::mapped_void(&c.data, 0), overlapped) == X_ERROR_IO_PENDING);
  CHECK(rex::system::XOverlappedGetResult(host) == X_ERROR_NO_SUCH_USER);
}

TEST_CASE("Deferred overlapped create reports pending, then the disposition and signals the event", "[runtime][content][xam][overlapped]") {
  Fixture f;
  auto* mem = f.kernel()->memory();
  const uint32_t guest = mem->SystemHeapAlloc(0x40);
  void* host = mem->TranslateVirtual(guest);
  std::memset(host, 0, 0x40);
  auto ev = rex::system::object_ref<rex::system::XEvent>(new rex::system::XEvent(f.kernel()));
  ev->Initialize(true, false);
  reinterpret_cast<rex::be<uint32_t>*>(host)[3] = ev->handle();  // overlapped.event
  rex::mapped_void overlapped(host, guest);

  Call c("SAVE", MakeData("slot1"));
  c.disposition = 0x1234;
  REQUIRE(c.Create(1, overlapped) == X_ERROR_IO_PENDING);
  CHECK(uint32_t(c.disposition) == 0);  // reset while pending
  CHECK(rex::system::XOverlappedGetResult(host) == X_ERROR_IO_PENDING);
  for (int i = 0; i < 500 && rex::system::XOverlappedGetResult(host) == X_ERROR_IO_PENDING; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(rex::system::XOverlappedGetResult(host) == X_ERROR_SUCCESS);
  CHECK(rex::system::XOverlappedGetLength(host) == kDispCreate);
  CHECK(uint32_t(c.disposition) == kDispCreate);
  uint64_t timeout = 0;
  CHECK(ev->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS);
  CHECK(c.Close() == X_ERROR_SUCCESS);
}
