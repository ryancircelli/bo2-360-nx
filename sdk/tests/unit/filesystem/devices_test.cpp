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

namespace {
// Synthetic read-only STFS (one hash-table slot per 170 blocks): slot 0 = hash table,
// slot n+1 = data block n. Block 0 = directory, 1 = a.txt, 2-3 = sub\b.bin (chained).
std::filesystem::path WriteSyntheticStfs(const std::filesystem::path& dir, const std::string& b_bin) {
  using namespace rex::filesystem;
  constexpr size_t kBlock = 0x1000, kHeader = 0xA000;  // round_up(header_size, 0x1000)
  std::vector<uint8_t> img(kHeader + 5 * kBlock, 0);
  auto* hdr = reinterpret_cast<StfsHeader*>(img.data());
  hdr->header.magic = XContentPackageType::kCon;
  hdr->header.header_size = sizeof(StfsHeader);
  auto& d = hdr->metadata.volume_descriptor.stfs;
  d.descriptor_length = sizeof(StfsVolumeDescriptor);
  d.flags.bits.read_only_format = 1;
  d.file_table_block_count = 1;
  d.set_file_table_block_number(0);
  d.total_block_count = 4;
  hdr->metadata.volume_type = XContentVolumeType::kStfs;

  auto* hash = reinterpret_cast<StfsHashTable*>(img.data() + kHeader);
  hash->entries[0].set_level0_next_block(0xFFFFFF);  // directory
  hash->entries[1].set_level0_next_block(0xFFFFFF);  // a.txt
  hash->entries[2].set_level0_next_block(3);         // b.bin chain 2 -> 3
  hash->entries[3].set_level0_next_block(0xFFFFFF);

  auto* dir_block = reinterpret_cast<StfsDirectoryBlock*>(img.data() + kHeader + 1 * kBlock);
  auto fill = [](StfsDirectoryEntry& e, const char* name, bool is_dir, uint16_t parent, uint32_t start,
                 uint32_t len) {
    std::memcpy(e.name, name, std::strlen(name));
    e.flags.name_length = std::strlen(name);
    e.flags.directory = is_dir;
    e.directory_index = parent;
    e.set_start_block_number(start);
    e.set_allocated_data_blocks((len + 0xFFF) / 0x1000);
    e.length = len;
  };
  fill(dir_block->entries[0], "sub", true, 0xFFFF, 0, 0);
  fill(dir_block->entries[1], "a.txt", false, 0xFFFF, 1, 5);
  fill(dir_block->entries[2], "b.bin", false, 0, 2, uint32_t(b_bin.size()));
  std::memcpy(img.data() + kHeader + 2 * kBlock, "hello", 5);
  std::memcpy(img.data() + kHeader + 3 * kBlock, b_bin.data(), std::min<size_t>(b_bin.size(), kBlock));
  if (b_bin.size() > kBlock) std::memcpy(img.data() + kHeader + 4 * kBlock, b_bin.data() + kBlock, b_bin.size() - kBlock);

  auto path = dir / "pkg";
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<char*>(img.data()), img.size());
  return path;
}
}  // namespace

TEST_CASE("STFS container mounts a synthetic package and reads chained files", "[vfs][stfs]") {
  Temp temp;
  std::string b(0x1000 + 100, 'x');
  b[0x1000 - 1] = 'A';  // last byte of first block
  b[0x1000] = 'B';      // first byte of second block
  auto path = WriteSyntheticStfs(temp.path, b);

  rex::filesystem::StfsContainerDevice device("\\Device\\Cdrom0", path);
  REQUIRE(device.Initialize());
  CHECK(device.is_read_only());
  CHECK(device.total_allocation_units() == 4);

  auto* a = device.ResolvePath("a.txt");
  REQUIRE(a != nullptr);
  CHECK(a->size() == 5);
  rex::filesystem::File* raw = nullptr;
  REQUIRE(a->Open(1, &raw) == X_STATUS_SUCCESS);
  std::unique_ptr<rex::filesystem::File, DestroyFile> file(raw);
  std::array<uint8_t, 8> buf{};
  size_t n = 0;
  REQUIRE(file->ReadSync(buf, 0, &n) == X_STATUS_SUCCESS);
  CHECK(n == 5);
  CHECK(std::memcmp(buf.data(), "hello", 5) == 0);

  auto* sub = device.ResolvePath("sub");
  REQUIRE(sub != nullptr);
  auto* bin = device.ResolvePath("sub\\b.bin");
  REQUIRE(bin != nullptr);
  CHECK(bin->size() == b.size());
  REQUIRE(bin->Open(1, &raw) == X_STATUS_SUCCESS);
  std::unique_ptr<rex::filesystem::File, DestroyFile> bfile(raw);
  std::vector<uint8_t> out(b.size());
  REQUIRE(bfile->ReadSync(out, 0, &n) == X_STATUS_SUCCESS);
  CHECK(n == b.size());
  CHECK(std::memcmp(out.data(), b.data(), b.size()) == 0);  // crosses the chained block boundary
  REQUIRE(bfile->ReadSync(std::span<uint8_t>(buf.data(), 2), 0x1000 - 1, &n) == X_STATUS_SUCCESS);
  CHECK((buf[0] == 'A' && buf[1] == 'B'));

  CHECK(device.ResolvePath("nope.txt") == nullptr);
}

TEST_CASE("Range cache keys on the absolute path so game: and update: with one relative path stay apart",
          "[vfs][host]") {
  using namespace rex::filesystem;
  Temp temp;
  constexpr size_t kSize = 512 * 1024;  // above switch_io_ranges_min_kb so the range cache is eligible
  for (const char* dir : {"game", "update"}) {
    std::filesystem::create_directories(temp.path / dir);
    std::ofstream out(temp.path / dir / "data.bin", std::ios::binary);
    std::string content(kSize, dir[0]);
    out.write(content.data(), content.size());
  }
  VirtualFileSystem vfs;
  REQUIRE(vfs.RegisterDevice(std::make_unique<HostPathDevice>("\\Device\\Game", temp.path / "game", true)));
  REQUIRE(vfs.RegisterDevice(std::make_unique<HostPathDevice>("\\Device\\Update", temp.path / "update", true)));
  REQUIRE(vfs.RegisterSymbolicLink("game:", "\\Device\\Game"));
  REQUIRE(vfs.RegisterSymbolicLink("update:", "\\Device\\Update"));
  for (int pass = 0; pass < 2; ++pass) {  // second pass would hit the cache
    for (auto [path, expect] : {std::pair{"game:\\data.bin", 'g'}, std::pair{"update:\\data.bin", 'u'}}) {
      rex::filesystem::File* raw = nullptr;
      auto* entry = vfs.ResolvePath(path);
      REQUIRE(entry);
      REQUIRE(entry->Open(FileAccess::kGenericRead, &raw) == X_STATUS_SUCCESS);
      FilePtr file(raw);
      std::vector<uint8_t> buf(kSize);
      size_t n = 0;
      REQUIRE(file->ReadSync(buf, 0, &n) == X_STATUS_SUCCESS);
      CHECK(n == kSize);
      CHECK(buf[0] == uint8_t(expect));
      CHECK(buf[kSize - 1] == uint8_t(expect));
    }
  }
}
