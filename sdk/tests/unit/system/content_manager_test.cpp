#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/xam/content_manager.h>

using rex::X_STATUS;
using rex::X_RESULT;

TEST_CASE("Content packages isolate owners and survive close reopen and delete", "[runtime][content]") {
  struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("rex-content-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  } temp;
  // Keep this test's close operation independent of any configured backup location.
  REQUIRE(rex::cvar::SetFlagFromCommandLine("content_backup_root", ""));
  rex::Runtime runtime(temp.path);
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  rex::system::xam::ContentManager manager(runtime.kernel_state(), temp.path / "content");
  rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = rex::system::XContentType::kSavedGame;
  data.title_id = 0x12345678;
  data.set_file_name("slot1");
  data.set_display_name(u"First slot");
  constexpr uint64_t owner = 0xE000000000000001ull;
  constexpr uint64_t other = 0xE000000000000002ull;
  CHECK_FALSE(manager.ContentExists(owner, data));
  REQUIRE(manager.CreateContent("save", owner, data) == X_ERROR_SUCCESS);
  CHECK(manager.ContentExists(owner, data));
  CHECK_FALSE(manager.ContentExists(other, data));
  CHECK(manager.CreateContent("SAVE", owner, data) == X_ERROR_ALREADY_EXISTS);
  CHECK(manager.DeleteContent(owner, data) == X_ERROR_ACCESS_DENIED);
  auto path = manager.GetOpenPackagePath("SAVE");
  REQUIRE_FALSE(path.empty());
  std::ofstream(path / "profile.dat") << "profile bytes";
  REQUIRE(manager.WriteContentHeaderFile(owner, data) == X_ERROR_SUCCESS);
  auto list = manager.ListContent(1, owner, rex::system::XContentType::kSavedGame, data.title_id);
  REQUIRE(list.size() == 1);
  CHECK(list[0].file_name() == "slot1");
  CHECK(list[0].display_name() == u"First slot");
  REQUIRE(manager.CloseContent("SAVE") == X_ERROR_SUCCESS);
  CHECK(manager.CloseContent("save") == X_ERROR_FILE_NOT_FOUND);
  uint32_t license = 99;
  REQUIRE(manager.OpenContent("save", owner, data, license) == X_ERROR_SUCCESS);
  CHECK(license == 0);
  CHECK(std::filesystem::file_size(manager.GetOpenPackagePath("save") / "profile.dat") == 13);
  REQUIRE(manager.CloseContent("save") == X_ERROR_SUCCESS);
  REQUIRE(manager.DeleteContent(owner, data) == X_ERROR_SUCCESS);
  CHECK_FALSE(manager.ContentExists(owner, data));
  CHECK(manager.ListContent(1, owner, rex::system::XContentType::kSavedGame, data.title_id).empty());
  CHECK(manager.DeleteContent(owner, data) == X_ERROR_FILE_NOT_FOUND);
  CHECK(manager.OpenContent("save", owner, data, license) == X_ERROR_FILE_NOT_FOUND);
}

TEST_CASE("ListContent hides and keeps .staging/.old leftovers of the reverted atomic save", "[runtime][content]") {
  auto root = std::filesystem::temp_directory_path() /
      ("rex-content-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  REQUIRE(rex::cvar::SetFlagFromCommandLine("content_backup_root", ""));
  rex::Runtime runtime(root);
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  rex::system::xam::ContentManager manager(runtime.kernel_state(), root / "content");
  rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = rex::system::XContentType::kSavedGame;
  data.title_id = 0x12345678;
  data.set_file_name("slot1");
  data.set_display_name(u"Slot");
  constexpr uint64_t owner = 0xE000000000000001ull;
  REQUIRE(manager.CreateContent("save", owner, data) == X_ERROR_SUCCESS);
  auto pkg = manager.GetOpenPackagePath("save");
  REQUIRE(manager.WriteContentHeaderFile(owner, data) == X_ERROR_SUCCESS);
  REQUIRE(manager.CloseContent("save") == X_ERROR_SUCCESS);
  auto staging = pkg.parent_path() / "slot1.staging", old = pkg.parent_path() / "slot1.old";
  for (auto& d : {staging, old}) {
    std::filesystem::create_directories(d);
    std::ofstream(d / "progress.dat") << "unsaved";
  }
  auto list = manager.ListContent(1, owner, rex::system::XContentType::kSavedGame, data.title_id);
  REQUIRE(list.size() == 1);
  CHECK(list[0].file_name() == "slot1");
  REQUIRE(manager.DeleteContent(owner, data) == X_ERROR_SUCCESS);
  CHECK(std::filesystem::exists(staging / "progress.dat"));  // never deleted: may hold the only copy of progress
  CHECK(std::filesystem::exists(old / "progress.dat"));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
