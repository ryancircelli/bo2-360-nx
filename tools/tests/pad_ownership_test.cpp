#include <rex/input/sdl/pad_ownership.h>

#include <cassert>
#include <filesystem>
#include <string>

using rex::input::sdl::MakePadIdentity;
using rex::input::sdl::PadOwnership;

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "claim") {
    const int instance = std::stoi(argv[2]);
    PadOwnership claimant(instance, argv[3]);
    return claimant.Owns("racing-device") ? 0 : 1;
  }
  assert(argc == 2);
  const std::filesystem::path registry = argv[1];
  PadOwnership instance0(0, registry);
  PadOwnership instance1(1, registry);

  const auto serial_identity = MakePadIdentity("guid-a", "serial-1", "usb:1");
  const auto path_identity = MakePadIdentity("guid-b", "", "usb:2");
  assert(serial_identity == "guid-a|serial|serial-1");
  assert(path_identity == "guid-b|path|usb:2");
  assert(MakePadIdentity("", "serial", "path").empty());
  assert(instance0.Owns(serial_identity));
  assert(!instance1.Owns(serial_identity));
  // The record remains stable when the same pad reconnects.
  assert(instance0.Owns(MakePadIdentity("guid-a", "serial-1", "usb:9")));
  assert(instance1.Owns(path_identity));
  assert(!instance0.Owns(path_identity));

  // Each process can claim four devices, for eight total.
  for (int i = 0; i < 3; ++i) {
    assert(instance0.Owns("pad0-" + std::to_string(i)));
    assert(instance1.Owns("pad1-" + std::to_string(i)));
  }
  assert(!instance0.Owns("overflow-0"));
  assert(!instance1.Owns("overflow-1"));
}
