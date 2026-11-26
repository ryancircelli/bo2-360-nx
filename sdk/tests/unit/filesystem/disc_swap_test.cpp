// Disc swap mount switch: DiscRoot layout rule and VirtualFileSystem::ReplaceDevice.

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>

namespace fs = std::filesystem;
using rex::filesystem::DiscRoot;
using rex::filesystem::HostPathDevice;
using rex::filesystem::VirtualFileSystem;

namespace {
constexpr char kDev[] = "\\Device\\Harddisk0\\Partition1";

fs::path MakeGameFolder() {
  auto data = fs::temp_directory_path() / "rex_disc_swap_test" / "data";
  fs::remove_all(data.parent_path());
  fs::create_directories(data / "game");
  fs::create_directories(data / "disc2");
  std::ofstream(data / "game" / "one.txt") << "1";
  std::ofstream(data / "disc2" / "two.txt") << "2";
  return data;
}

std::unique_ptr<HostPathDevice> Mount(const fs::path& root) {
  auto d = std::make_unique<HostPathDevice>(kDev, root, true);
  REQUIRE(d->Initialize());
  return d;
}
}  // namespace

TEST_CASE("DiscRoot finds the boot disc and data/disc<N>", "[disc_swap]") {
  const auto data = MakeGameFolder();
  CHECK(DiscRoot(data / "game", 1, 1) == data / "game");
  CHECK(DiscRoot(data / "game" / "", 1, 1) == data / "game");  // trailing separator
  CHECK(DiscRoot(data / "game", 2, 1) == data / "disc2");
  CHECK(DiscRoot(data / "game", 3, 1).empty());
  CHECK(DiscRoot(data / "game", 1, 2).empty());  // booted from disc 2, no disc1 folder
}

TEST_CASE("ReplaceDevice switches game: and keeps old entries alive", "[disc_swap]") {
  const auto data = MakeGameFolder();
  VirtualFileSystem vfs;
  REQUIRE(vfs.RegisterDevice(Mount(data / "game")));
  REQUIRE(vfs.RegisterSymbolicLink("game:", kDev));

  auto* one = vfs.ResolvePath("game:\\one.txt");
  REQUIRE(one != nullptr);
  CHECK(vfs.ResolvePath("game:\\two.txt") == nullptr);

  REQUIRE(vfs.ReplaceDevice(Mount(data / "disc2")));
  CHECK(vfs.ResolvePath("game:\\two.txt") != nullptr);
  CHECK(vfs.ResolvePath("game:\\one.txt") == nullptr);
  CHECK(one->name() == "one.txt");  // entry from the retired device is still valid

  REQUIRE(vfs.ReplaceDevice(Mount(data / "game")));  // and back
  CHECK(vfs.ResolvePath("game:\\one.txt") != nullptr);

  auto stray = std::make_unique<HostPathDevice>("\\Device\\Cdrom9", data / "game", true);
  CHECK_FALSE(vfs.ReplaceDevice(std::move(stray)));
}
