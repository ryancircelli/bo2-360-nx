#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

using rex::X_STATUS;

namespace {
struct Temp {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("rex-devices-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { std::filesystem::create_directories(path); }
  ~Temp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct DestroyFile {
  void operator()(rex::filesystem::File* file) const { file->Destroy(); }
};
using FilePtr = std::unique_ptr<rex::filesystem::File, DestroyFile>;
}

TEST_CASE("Host devices round trip saves truncate overwrite and resolve cache aliases", "[vfs][host]") {
  using namespace rex::filesystem;
  Temp temp;
  VirtualFileSystem vfs;
  auto device = std::make_unique<HostPathDevice>("\\Device\\UnitTest", temp.path, false);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("cache:", "\\Device\\UnitTest"));
  FileAction action{};
  rex::filesystem::File* raw = nullptr;
  REQUIRE(vfs.OpenFile(nullptr, "cache:\\save.bin", FileDisposition::kCreate,
                       FileAccess::kGenericRead | FileAccess::kGenericWrite,
                       false, true, &raw, &action) == X_STATUS_SUCCESS);
  FilePtr save(raw);
  CHECK(action == FileAction::kCreated);
  std::array<uint8_t, 4> data{1, 2, 3, 4};
  size_t bytes = 0;
  REQUIRE(save->WriteSync(data, 0, &bytes) == X_STATUS_SUCCESS);
  CHECK(bytes == data.size());
  REQUIRE(save->Flush() == X_STATUS_SUCCESS);
  save.reset();
  auto entry = vfs.ResolvePath("cache:\\SAVE.BIN");
  REQUIRE(entry);
  REQUIRE(entry->Open(FileAccess::kGenericRead, &raw) == X_STATUS_SUCCESS);
  FilePtr read(raw);
  std::array<uint8_t, 8> output{};
  REQUIRE(read->ReadSync(output, 0, &bytes) == X_STATUS_SUCCESS);
  CHECK(bytes == 4);
  CHECK(std::equal(data.begin(), data.end(), output.begin()));
  read.reset();
  REQUIRE(vfs.OpenFile(nullptr, "cache:\\save.bin", FileDisposition::kOverwrite,
                       FileAccess::kGenericWrite, false, true, &raw, &action) == X_STATUS_SUCCESS);
  FilePtr overwrite(raw);
  CHECK(action == FileAction::kOverwritten);
  overwrite.reset();
  CHECK(std::filesystem::file_size(temp.path / "save.bin") == 0);
  REQUIRE(entry->Rename("cache:/renamed.bin") == X_STATUS_SUCCESS);
  CHECK(std::filesystem::exists(temp.path / "renamed.bin"));
  CHECK_FALSE(std::filesystem::exists(temp.path / "save.bin"));
  CHECK(vfs.ResolvePath("cache:\\renamed.bin") == entry);
  REQUIRE(vfs.UnregisterSymbolicLink("cache:"));
  CHECK_FALSE(vfs.ResolvePath("cache:\\renamed.bin"));
}

TEST_CASE("Read-only host devices reject creation and truncation", "[vfs][host]") {
  using namespace rex::filesystem;
  Temp temp;
  std::ofstream(temp.path / "existing") << "keep";
  HostPathDevice device("\\Device\\ReadOnly", temp.path, true);
  REQUIRE(device.Initialize());
  auto root = device.ResolvePath("");
  REQUIRE(root);
  CHECK_FALSE(root->CreateEntry("new", kFileAttributeNormal));
  auto entry = device.ResolvePath("existing");
  REQUIRE(entry);
  CHECK_FALSE(entry->Truncate());
  CHECK(std::filesystem::file_size(temp.path / "existing") == 4);
}

TEST_CASE("Synthetic disc image enumerates and reads a case-insensitive file", "[vfs][disc]") {
  using namespace rex::filesystem;
  Temp temp;
  std::vector<uint8_t> image(35 * 2048);
  auto put32 = [&](size_t offset, uint32_t value) { std::memcpy(image.data() + offset, &value, 4); };
  std::memcpy(image.data() + 32 * 2048, "MICROSOFT*XBOX*MEDIA", 20);
  put32(32 * 2048 + 20, 33);
  put32(32 * 2048 + 24, 2048);
  put32(33 * 2048 + 4, 34);
  put32(33 * 2048 + 8, 4);
  image[33 * 2048 + 13] = 8;
  std::memcpy(image.data() + 33 * 2048 + 14, "test.bin", 8);
  std::memcpy(image.data() + 34 * 2048, "DATA", 4);
  const auto path = temp.path / "test.iso";
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data()), image.size());
  }
  DiscImageDevice device("\\Device\\Cdrom0", path);
  REQUIRE(device.Initialize());
  CHECK(device.file_count() == 1);
  CHECK(device.total_file_size() == 4);
  auto entry = device.ResolvePath("TEST.BIN");
  REQUIRE(entry);
  CHECK(entry->is_read_only());
  rex::filesystem::File* raw = nullptr;
  REQUIRE(entry->Open(FileAccess::kGenericRead, &raw) == X_STATUS_SUCCESS);
  FilePtr file(raw);
  std::array<uint8_t, 8> data{};
  size_t bytes = 0;
  REQUIRE(file->ReadSync(data, 2, &bytes) == X_STATUS_SUCCESS);
  CHECK(bytes == 2);
  CHECK(data[0] == 'T');
  CHECK(data[1] == 'A');
  CHECK(file->ReadSync(data, 4, &bytes) == X_STATUS_END_OF_FILE);
}

TEST_CASE("STFS header reader rejects missing short and invalid magic packages", "[vfs][stfs]") {
  Temp temp;
  auto path = temp.path / "package";
  CHECK_FALSE(rex::filesystem::StfsContainerDevice::ReadPackageHeader(path));
  { std::ofstream(path, std::ios::binary) << "CON "; }
  CHECK_FALSE(rex::filesystem::StfsContainerDevice::ReadPackageHeader(path));
  {
    std::vector<char> zeroes(sizeof(rex::filesystem::StfsHeader));
    std::ofstream out(path, std::ios::binary);
    out.write(zeroes.data(), zeroes.size());
  }
  CHECK_FALSE(rex::filesystem::StfsContainerDevice::ReadPackageHeader(path));
}
