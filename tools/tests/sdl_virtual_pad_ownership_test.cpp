#include <SDL3/SDL.h>

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/input/sdl/pad_ownership.h>

using rex::input::sdl::MakePadIdentity;
using rex::input::sdl::PadOwnership;

namespace {

struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  std::string identity;
};

VirtualPad AttachVirtualPad(int slot) {
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.vendor_id = 0x1209;
  desc.product_id = static_cast<Uint16>(0x3000 + slot);
  desc.naxes = 4;
  desc.nbuttons = 16;
  desc.name = "BO2 SDL virtual pad";

  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  assert(id != 0);
  SDL_Joystick* joystick = SDL_OpenJoystick(id);
  assert(joystick != nullptr);

  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));
  // SDL's virtual-joystick API exposes neither serial nor path. Use a stable
  // fixture serial for each virtual device, as physical pads supply in SDL.
  const std::string serial = "test-virtual-serial-" + std::to_string(slot);
  return {id, joystick, MakePadIdentity(guid, serial, "")};
}

void Detach(VirtualPad& pad) {
  SDL_CloseJoystick(pad.joystick);
  assert(SDL_DetachVirtualJoystick(pad.id));
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  assert(SDL_Init(SDL_INIT_JOYSTICK));

  const std::filesystem::path registry = argv[1];
  PadOwnership instance0(0, registry);
  PadOwnership instance1(1, registry);
  std::vector<VirtualPad> pads;
  pads.reserve(8);

  for (int slot = 0; slot < 8; ++slot) {
    auto pad = AttachVirtualPad(slot);
    assert(SDL_IsJoystickVirtual(pad.id));
    assert(!pad.identity.empty());
    PadOwnership& owner = slot < 4 ? instance0 : instance1;
    PadOwnership& other = slot < 4 ? instance1 : instance0;
    assert(owner.Owns(pad.identity));
    assert(!other.Owns(pad.identity));
    pads.push_back(std::move(pad));
  }

  // A detached and reattached device with the same fixture identity returns
  // to the original copy, even though SDL assigns a new transient instance ID.
  const auto retained_identity = pads[2].identity;
  Detach(pads[2]);
  pads[2] = AttachVirtualPad(2);
  assert(instance0.Owns(pads[2].identity));
  assert(!instance1.Owns(pads[2].identity));
  assert(pads[2].identity == retained_identity);

  auto overflow = AttachVirtualPad(8);
  assert(!instance0.Owns(overflow.identity));
  assert(!instance1.Owns(overflow.identity));
  Detach(overflow);

  for (auto& pad : pads) Detach(pad);
  SDL_Quit();
}
