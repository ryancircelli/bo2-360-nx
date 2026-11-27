#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/xtypes.h>

using rex::X_STATUS;
using rex::filesystem::HostPathDevice;

namespace {

std::filesystem::path MakeDir() {
  auto dir = std::filesystem::temp_directory_path() / "rex_host_path_rename_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "sub");
  return dir;
}

void Write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

size_t CountChildren(rex::filesystem::Entry* dir, const std::string& name) {
  size_t n = 0;
  for (const auto& child : dir->children()) {
    n += child->name() == name;
  }
  return n;
}

}  // namespace

TEST_CASE("XAPI MoveFile target with \\??\\ prefix renames on the host and in the tree") {
  auto dir = MakeDir();
  Write(dir / "a.tmp", "save");
  HostPathDevice device("\\Device\\Content\\1", dir, false);
  REQUIRE(device.Initialize());

  auto* entry = device.ResolvePath("a.tmp");
  REQUIRE(entry);
  REQUIRE(entry->Rename("\\??\\savegame:\\b.sav") == X_STATUS_SUCCESS);

  CHECK(entry->name() == "b.sav");
  CHECK(entry->path() == "b.sav");
  CHECK(device.ResolvePath("b.sav") == entry);
  CHECK(Read(dir / "b.sav") == "save");
  CHECK_FALSE(std::filesystem::exists(dir / "a.tmp"));
  std::filesystem::remove_all(dir);
}

TEST_CASE("Rename over an existing file leaves one entry with the new contents") {
  auto dir = MakeDir();
  Write(dir / "new.tmp", "new");
  Write(dir / "save.dat", "old");
  HostPathDevice device("\\Device\\Content\\1", dir, false);
  REQUIRE(device.Initialize());

  auto* entry = device.ResolvePath("new.tmp");
  REQUIRE(entry);
  REQUIRE(entry->Rename("savegame:\\save.dat") == X_STATUS_SUCCESS);

  auto* root = device.ResolvePath("");
  REQUIRE(root);
  CHECK(CountChildren(root, "save.dat") == 1);
  CHECK(CountChildren(root, "new.tmp") == 0);
  CHECK(device.ResolvePath("save.dat") == entry);
  CHECK(Read(dir / "save.dat") == "new");
  std::filesystem::remove_all(dir);
}

TEST_CASE("Rename into a subdirectory moves the entry under that directory") {
  auto dir = MakeDir();
  Write(dir / "a.tmp", "x");
  HostPathDevice device("\\Device\\Content\\1", dir, false);
  REQUIRE(device.Initialize());

  auto* entry = device.ResolvePath("a.tmp");
  REQUIRE(entry);
  REQUIRE(entry->Rename("savegame:\\sub\\a.sav") == X_STATUS_SUCCESS);

  auto* sub = device.ResolvePath("sub");
  REQUIRE(sub);
  CHECK(entry->parent() == sub);
  CHECK(device.ResolvePath("sub\\a.sav") == entry);
  CHECK(CountChildren(device.ResolvePath(""), "a.tmp") == 0);
  CHECK(Read(dir / "sub" / "a.sav") == "x");
  std::filesystem::remove_all(dir);
}

TEST_CASE("Open-if of a host file missing from the tree opens it instead of truncating it") {
  auto dir = MakeDir();
  rex::filesystem::VirtualFileSystem vfs;
  auto device = std::make_unique<HostPathDevice>("\\Device\\Content\\1", dir, false);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  Write(dir / "late.sav", "progress");  // appears after the mount walked the directory

  rex::filesystem::File* file = nullptr;
  rex::filesystem::FileAction action{};
  REQUIRE(vfs.OpenFile(nullptr, "\\Device\\Content\\1\\late.sav",
                       rex::filesystem::FileDisposition::kOpenIf,
                       rex::filesystem::FileAccess::kFileReadData, false, true, &file,
                       &action) == X_STATUS_SUCCESS);
  CHECK(action == rex::filesystem::FileAction::kOpened);
  CHECK(Read(dir / "late.sav") == "progress");
  file->Destroy();
  std::filesystem::remove_all(dir);
}
