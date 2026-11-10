/**
 * @file        input/sdl/pad_location_mac.mm
 * @brief       Finds which USB port an SDL gamepad is plugged into (macOS).
 *
 * SDL gives wired Xbox pads on macOS no serial and no path: they are driven
 * through the GameController framework, which hides the HID device. The port
 * is still in IOKit, as the LocationID of the pad's HID device. To find that
 * device for an SDL pad:
 *  - HIDAPI pads: SDL_GetGamepadPath is "DevSrvsID:<registry entry id>".
 *  - GameController pads: SDL forwards SDL_SetGamepadPlayerIndex to
 *    GCController.playerIndex, so a player index no other controller holds
 *    picks out this pad's GCController; its HID services (a private
 *    GCController property, read defensively) carry the registry entry id.
 * Every failure leaves the location empty with a note, and the pad falls back
 * to GUID plus connection order, as before.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "pad_location.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <fmt/format.h>

#include <SDL3/SDL_joystick.h>

namespace rex::input {

namespace {

std::optional<uint32_t> LocationForRegistryId(uint64_t entry_id) {
  io_service_t service =
      IOServiceGetMatchingService(MACH_PORT_NULL, IORegistryEntryIDMatching(entry_id));
  if (!service) {
    return std::nullopt;
  }
  std::optional<uint32_t> result;
  // The HID device carries "LocationID"; the USB device above it "locationID".
  for (CFStringRef key : {CFSTR(kIOHIDLocationIDKey), CFSTR("locationID")}) {
    CFTypeRef ref = IORegistryEntrySearchCFProperty(
        service, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (!ref) {
      continue;
    }
    uint32_t value = 0;
    if (CFGetTypeID(ref) == CFNumberGetTypeID() &&
        CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, &value) &&
        value != 0) {
      result = value;
    }
    CFRelease(ref);
    if (result) {
      break;
    }
  }
  IOObjectRelease(service);
  return result;
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

std::optional<uint32_t> LocationForGCController(GCController* c, std::string* why) {
  @try {
    SEL selector = NSSelectorFromString(@"hidServices");
    if (![c respondsToSelector:selector]) {
      *why = "this macOS has no GCController hidServices";
      return std::nullopt;
    }
    id services = [c valueForKey:@"hidServices"];
    if (![services isKindOfClass:[NSArray class]] || [services count] == 0) {
      *why = "GCController has no HID services (Bluetooth or not yet attached?)";
      return std::nullopt;
    }
    for (id service in static_cast<NSArray*>(services)) {
      if (![service respondsToSelector:NSSelectorFromString(@"registryID")]) {
        continue;
      }
      id number = [service valueForKey:@"registryID"];
      if (![number isKindOfClass:[NSNumber class]]) {
        continue;
      }
      if (auto location = LocationForRegistryId([number unsignedLongLongValue])) {
        return location;
      }
    }
    *why = "GameController HID service has no LocationID";
  } @catch (NSException* e) {
    *why = fmt::format("GCController lookup threw {}", e.name.UTF8String);
  }
  return std::nullopt;
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
      if (auto location = LocationForRegistryId(entry_id)) {
        out.location = fmt::format("{:08x}", *location);
        out.note = "IOKit LocationID via SDL path";
      } else {
        out.note = "IOKit has no LocationID for SDL path";
      }
      return out;
    }
    if (!IsGameControllerPad(pad)) {
      out.note = "SDL backend gives no path";
      return out;
    }
    std::string why;
    if (GCController* c = FindGCController(pad, &why)) {
      if (auto location = LocationForGCController(c, &why)) {
        out.location = fmt::format("{:08x}", *location);
        out.note = "IOKit LocationID via GameController";
        return out;
      }
    }
    out.note = why;
  }
  return out;
}

}  // namespace rex::input
