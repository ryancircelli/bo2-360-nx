/**
 * @file        input/sdl/pad_location_mac.mm
 * @brief       Finds which USB port or Bluetooth device an SDL gamepad is (macOS).
 *
 * SDL gives Xbox pads on macOS no serial and no path: they are driven through
 * the GameController framework, which hides the HID device. IOKit still has
 * it: a USB pad's HID device carries its port's LocationID, a Bluetooth pad's
 * carries (on the HID device bluetoothd creates) its DeviceAddress. To find
 * that device for an SDL pad:
 *  - HIDAPI pads: SDL_GetGamepadPath is "DevSrvsID:<registry entry id>".
 *  - GameController pads: SDL forwards SDL_SetGamepadPlayerIndex to
 *    GCController.playerIndex, so a player index no other controller holds
 *    picks out this pad's GCController; its HID services (a private
 *    GCController property, read defensively) carry the registry entry id,
 *    and failing that its physicalDeviceUniqueID (also private) is matched
 *    against the PhysicalDeviceUniqueID of IOKit's HID devices.
 * A Bluetooth HID device also has a LocationID, but it is not a port and is
 * not used.
 * Every failure leaves the location empty with a note, and the pad falls back
 * to GUID plus connection order, as before.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/sdl/pad_location.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <fmt/format.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <rex/input/device.h>

namespace rex::input {

namespace {

std::string CFToString(CFTypeRef ref) {
  if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) {
    return {};
  }
  NSString* text = (__bridge NSString*)ref;
  return text.UTF8String ? text.UTF8String : "";
}

// The property on the entry or the nearest parent that has it, retained.
CFTypeRef SearchUp(io_registry_entry_t entry, CFStringRef key) {
  return IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault,
                                         kIORegistryIterateRecursively |
                                             kIORegistryIterateParents);
}

std::string StringUp(io_registry_entry_t entry, CFStringRef key) {
  CFTypeRef ref = SearchUp(entry, key);
  std::string text = CFToString(ref);
  if (ref) {
    CFRelease(ref);
  }
  return text;
}

std::optional<uint32_t> NumberUp(io_registry_entry_t entry, CFStringRef key) {
  CFTypeRef ref = SearchUp(entry, key);
  std::optional<uint32_t> result;
  uint32_t value = 0;
  if (ref && CFGetTypeID(ref) == CFNumberGetTypeID() &&
      CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, &value)) {
    result = value;
  }
  if (ref) {
    CFRelease(ref);
  }
  return result;
}

bool IsBluetoothTransport(std::string_view transport) {
  return transport.find("Bluetooth") != std::string_view::npos ||
         transport.find("bluetooth") != std::string_view::npos;
}

// What identifies the HID device at this registry entry: its USB port, or its
// Bluetooth address. `via` says how the entry was reached, for the note.
PadLocation ReadEntry(io_registry_entry_t entry, std::string_view via) {
  PadLocation out;
  out.transport = StringUp(entry, CFSTR(kIOHIDTransportKey));
  if (IsBluetoothTransport(out.transport)) {
    out.bt_id = StringUp(entry, CFSTR(kIOHIDPhysicalDeviceUniqueIDKey));
    // bluetoothd's HID device has "DeviceAddress"; classic Bluetooth HID
    // devices often carry the address as their serial number instead.
    for (CFStringRef key : {CFSTR("DeviceAddress"), CFSTR(kIOHIDSerialNumberKey)}) {
      out.bt_address = NormalizeBluetoothAddress(StringUp(entry, key));
      if (!out.bt_address.empty()) {
        out.note = fmt::format("{} address via {}", out.transport, via);
        return out;
      }
    }
    out.note = fmt::format("{} device via {} shows no address", out.transport, via);
    return out;
  }
  // The HID device carries "LocationID"; the USB device above it "locationID".
  for (CFStringRef key : {CFSTR(kIOHIDLocationIDKey), CFSTR("locationID")}) {
    if (auto value = NumberUp(entry, key); value && *value != 0) {
      out.location = fmt::format("{:08x}", *value);
      out.note = fmt::format("IOKit LocationID via {}", via);
      return out;
    }
  }
  out.note = fmt::format("IOKit has no LocationID via {}", via);
  return out;
}

std::optional<PadLocation> ReadRegistryId(uint64_t entry_id, std::string_view via) {
  io_service_t service =
      IOServiceGetMatchingService(MACH_PORT_NULL, IORegistryEntryIDMatching(entry_id));
  if (!service) {
    return std::nullopt;
  }
  PadLocation out = ReadEntry(service, via);
  IOObjectRelease(service);
  return out;
}

bool Found(const PadLocation& location) {
  return !location.location.empty() || !location.bt_address.empty();
}

template <typename Fn>
void ForEachHidDevice(Fn&& fn) {
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOHIDDeviceKey), &it) !=
      KERN_SUCCESS) {
    return;
  }
  while (io_service_t service = IOIteratorNext(it)) {
    const bool more = fn(service);
    IOObjectRelease(service);
    if (!more) {
      break;
    }
  }
  IOObjectRelease(it);
}

// SDL marks GameController-framework (MFi) pads with 'm' in GUID byte 14.
bool IsGameControllerPad(SDL_Gamepad* pad) {
  const SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad));
  return guid.data[14] == 'm';
}

GCController* UniqueWithIndex(NSArray<GCController*>* all, NSInteger index) {
  GCController* found = nil;
  for (GCController* c in all) {
    if (c.playerIndex == index) {
      if (found) {
        return nil;
      }
      found = c;
    }
  }
  return found;
}

GCController* FindGCController(SDL_Gamepad* pad, std::string* why) {
  NSArray<GCController*>* all = [GCController controllers];
  if (all.count == 0) {
    *why = "GameController lists no controllers";
    return nil;
  }
  const int current = SDL_GetGamepadPlayerIndex(pad);
  if (current >= 0) {
    if (GCController* c = UniqueWithIndex(all, current)) {
      return c;
    }
  }
  for (NSInteger tag = GCControllerPlayerIndex1; tag <= GCControllerPlayerIndex4; tag++) {
    const bool held = [all indexOfObjectPassingTest:^BOOL(GCController* c, NSUInteger, BOOL*) {
                        return c.playerIndex == tag;
                      }] != NSNotFound;
    if (held) {
      continue;
    }
    SDL_SetGamepadPlayerIndex(pad, static_cast<int>(tag));
    GCController* c = UniqueWithIndex([GCController controllers], tag);
    SDL_SetGamepadPlayerIndex(pad, current);
    if (c) {
      return c;
    }
    *why = "player index tag did not reach a GameController pad";
    return nil;
  }
  *why = "no free player index to tag the GameController pad with";
  return nil;
}

id PrivateValue(GCController* c, NSString* key) {
  if (![c respondsToSelector:NSSelectorFromString(key)]) {
    return nil;
  }
  return [c valueForKey:key];
}

PadLocation LocationForGCController(GCController* c) {
  PadLocation out;
  std::string why = "this macOS has no GCController hidServices";
  @try {
    id services = PrivateValue(c, @"hidServices");
    if ([services isKindOfClass:[NSArray class]]) {
      why = "GCController has no HID services";
      for (id service in static_cast<NSArray*>(services)) {
        if (![service respondsToSelector:NSSelectorFromString(@"registryID")]) {
          continue;
        }
        id number = [service valueForKey:@"registryID"];
        if (![number isKindOfClass:[NSNumber class]]) {
          continue;
        }
        if (auto read = ReadRegistryId([number unsignedLongLongValue], "GameController")) {
          out = std::move(*read);
          if (Found(out)) {
            return out;
          }
          why = out.note;
        }
      }
    }
    // No usable HID service: match the controller's unique id to a HID device.
    id unique = PrivateValue(c, @"physicalDeviceUniqueID");
    if ([unique isKindOfClass:[NSString class]] && [unique length] > 0) {
      const std::string wanted = [unique UTF8String];
      bool matched = false;
      ForEachHidDevice([&](io_service_t service) {
        CFTypeRef ref = IORegistryEntryCreateCFProperty(
            service, CFSTR(kIOHIDPhysicalDeviceUniqueIDKey), kCFAllocatorDefault, 0);
        const bool same = CFToString(ref) == wanted;
        if (ref) {
          CFRelease(ref);
        }
        if (!same) {
          return true;
        }
        PadLocation read = ReadEntry(service, "GameController unique id");
        matched = true;
        if (Found(read)) {
          out = std::move(read);
          return false;
        }
        why = read.note;
        return true;
      });
      if (Found(out)) {
        return out;
      }
      if (!matched) {
        why += "; no HID device has its unique id";
      }
    }
  } @catch (NSException* e) {
    why = fmt::format("GCController lookup threw {}", e.name.UTF8String);
  }
  out = {};
  out.note = why;
  return out;
}

}  // namespace

PadLocation FindPadLocation(SDL_Gamepad* pad) {
  PadLocation out;
  if (!pad) {
    return out;
  }
  @autoreleasepool {
    const char* path = SDL_GetGamepadPath(pad);
    const std::string_view path_view = path ? path : "";
    if (path_view.starts_with("DevSrvsID:")) {
      const uint64_t entry_id = std::strtoull(path + 10, nullptr, 10);
      if (auto read = ReadRegistryId(entry_id, "SDL path")) {
        return std::move(*read);
      }
      out.note = "IOKit has no device for SDL path";
      return out;
    }
    if (!IsGameControllerPad(pad)) {
      out.note = "SDL backend gives no path";
      return out;
    }
    std::string why;
    if (GCController* c = FindGCController(pad, &why)) {
      return LocationForGCController(c);
    }
    out.note = why;
  }
  return out;
}

std::vector<HidIdentity> ListHidIdentities() {
  std::vector<HidIdentity> out;
  @autoreleasepool {
    ForEachHidDevice([&](io_service_t service) {
      HidIdentity item;
      CFTypeRef product = IORegistryEntryCreateCFProperty(service, CFSTR(kIOHIDProductKey),
                                                          kCFAllocatorDefault, 0);
      item.product = CFToString(product);
      if (product) {
        CFRelease(product);
      }
      for (auto [key, field] : {std::pair{CFSTR(kIOHIDVendorIDKey), &item.vendor_id},
                                std::pair{CFSTR(kIOHIDProductIDKey), &item.product_id}}) {
        CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
        if (ref && CFGetTypeID(ref) == CFNumberGetTypeID()) {
          CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, field);
        }
        if (ref) {
          CFRelease(ref);
        }
      }
      IORegistryEntryGetRegistryEntryID(service, &item.registry_id);
      item.location = ReadEntry(service, "IOKit");
      out.push_back(std::move(item));
      return true;
    });
  }
  return out;
}

}  // namespace rex::input
