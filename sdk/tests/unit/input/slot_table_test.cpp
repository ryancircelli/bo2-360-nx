#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/input/slot_table.h>

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::SlotTable;

namespace {

DeviceInfo Pad(uint64_t id, std::string guid, std::string serial = {}) {
  DeviceInfo info;
  info.id = static_cast<DeviceId>(id);
  info.name = "Xbox Wireless Controller";
  info.guid = std::move(guid);
  info.serial = std::move(serial);
  info.vendor_id = 0x045e;
  info.product_id = 0x0b13;
  return info;
}

struct Table {
  SlotTable table;
  std::vector<DeviceInfo> live;

  uint32_t Connect(DeviceInfo info) {
    info.ordinal = table.AssignOrdinal(info, live);
    live.push_back(info);
    return info.ordinal;
  }

  void Disconnect(uint64_t id) {
    for (auto it = live.begin(); it != live.end(); ++it) {
      if (it->id == static_cast<DeviceId>(id)) {
        table.OnDisconnected(*it);
        live.erase(it);
        return;
      }
    }
  }
};

}  // namespace

TEST_CASE("A controller changing from USB to Bluetooth reclaims its player", "[input][slots]") {
  Table t;
  DeviceInfo wired = Pad(1, "usb-guid");
  wired.location = "14200000";
  DeviceInfo other = Pad(2, "other-guid", "other-serial");
  other.product_id = 0x0b12;
  CHECK(t.Connect(wired) == 0);
  CHECK(t.Connect(other) == 1);
  t.Disconnect(1);

  DeviceInfo bluetooth = Pad(3, "bluetooth-guid", "newly-reported-serial");
  bluetooth.transport = "bluetooth";
  CHECK(t.Connect(bluetooth) == 0);
  CHECK(t.live[0].ordinal == 1);
  CHECK(t.table.Reservations().empty());
}

TEST_CASE("An Xbox pad whose product id changes with the transport reclaims its player", "[input][slots]") {
  Table t;
  DeviceInfo wired = Pad(1, "usb-guid");
  wired.product_id = 0x0b12;
  wired.location = "14200000";
  CHECK(t.Connect(wired) == 0);
  t.Disconnect(1);

  DeviceInfo bluetooth = Pad(2, "bluetooth-guid");
  bluetooth.bt_address = "a0:b1:c2:d3:e4:f5";
  CHECK(t.Connect(bluetooth) == 0);
}

TEST_CASE("Model fallback never takes another serial's reservation", "[input][slots]") {
  Table t;
  CHECK(t.Connect(Pad(1, "usb-guid", "serial-a")) == 0);
  t.Disconnect(1);

  CHECK(t.Connect(Pad(2, "bluetooth-guid", "serial-b")) == 1);
  CHECK(t.Connect(Pad(3, "usb-guid", "serial-a")) == 0);
}

TEST_CASE("A replayed pad shares player 1 with a route pad", "[input][slots]") {
  Table t;
  DeviceInfo route = Pad(1, "route-guid", "script-pad-1");
  route.fixed_user = 0;
  DeviceInfo replay = Pad(2, "replay-guid", "recorded-pad-1");
  replay.fixed_user = 0;
  CHECK(t.Connect(route) == 0);
  CHECK(t.Connect(replay) == 0);
  CHECK(t.Connect(Pad(3, "real-guid", "real-serial")) == 1);
}

using rex::input::ParseSlotPins;
using rex::input::ParseVirtualPadPolicy;
using rex::input::PinMatches;
using rex::input::PlacementOrder;
using rex::input::VirtualPadPolicy;

TEST_CASE("Virtual pad policy names parse and anything else is rejected", "[input][slots]") {
  CHECK(ParseVirtualPadPolicy("after") == VirtualPadPolicy::kAfterPhysical);
  CHECK(ParseVirtualPadPolicy("normal") == VirtualPadPolicy::kNormal);
  CHECK(ParseVirtualPadPolicy("ignore") == VirtualPadPolicy::kIgnore);
  CHECK_FALSE(ParseVirtualPadPolicy("Ignore ").has_value());
  CHECK_FALSE(ParseVirtualPadPolicy("").has_value());
}

TEST_CASE("Slot pins are 1-based, split on ; or , and malformed entries are skipped with a message", "[input][slots]") {
  std::string error;
  auto pins = ParseSlotPins("1=abc;2=045e:0b13,0=bad;x=y;3", &error);
  REQUIRE(pins.size() == 2);
  CHECK(pins[0].user == 0);
  CHECK(pins[0].match == "abc");
  CHECK(pins[1].user == 1);
  CHECK(pins[1].match == "045e:0b13");
  CHECK_FALSE(error.empty());
  error.clear();
  CHECK(ParseSlotPins("", &error).empty());
  CHECK(error.empty());
}

TEST_CASE("PinMatches accepts serial, guid, vid:pid, port, bluetooth address and name fragments", "[input][slots]") {
  DeviceInfo pad = Pad(1, "GUID-1", "SER123");
  pad.location = "14200000";
  pad.bt_address = "aa:bb:cc:dd:ee:ff";
  CHECK(PinMatches("ser123", pad));
  CHECK(PinMatches("guid-1", pad));
  CHECK(PinMatches("045E:0B13", pad));
  CHECK(PinMatches("14200000", pad));
  CHECK(PinMatches("port:14200000", pad));
  CHECK(PinMatches("AA:BB:CC:DD:EE:FF", pad));
  CHECK(PinMatches("bt:aa:bb:cc:dd:ee:ff", pad));
  CHECK(PinMatches("wireless", pad));
  CHECK_FALSE(PinMatches("playstation", pad));
  CHECK_FALSE(PinMatches("045e:0b12", pad));
  CHECK_FALSE(PinMatches("", pad));
}

TEST_CASE("A pin gives a pad its player, and an unpinned pad avoids pinned players", "[input][slots]") {
  Table t;
  t.table.SetPins(ParseSlotPins("1=pin-serial"));
  CHECK(t.Connect(Pad(1, "g1", "other")) == 1);  // player 1 is pinned to someone else
  CHECK(t.Connect(Pad(2, "g2", "pin-serial")) == 0);
}

TEST_CASE("A script pad with a fixed user holds that player", "[input][slots]") {
  Table t;
  DeviceInfo script = Pad(1, "script");
  script.fixed_user = 2;
  script.ordinal = 2;
  t.live.push_back(script);
  CHECK(t.Connect(Pad(2, "a", "sa")) == 0);
  CHECK(t.Connect(Pad(3, "b", "sb")) == 1);
  CHECK(t.Connect(Pad(4, "c", "sc")) == 3);  // 2 is taken by the script pad
}

TEST_CASE("Virtual pads skip player 1 unless the policy says otherwise", "[input][slots]") {
  {
    Table t;  // default: after physical
    DeviceInfo virt = Pad(1, "v", "sv");
    virt.virtual_device = true;
    CHECK(t.Connect(virt) == 1);
    CHECK(t.Connect(Pad(2, "p", "sp")) == 0);
  }
  {
    Table t;
    t.table.SetVirtualPolicy(VirtualPadPolicy::kNormal);
    DeviceInfo virt = Pad(1, "v", "sv");
    virt.virtual_device = true;
    CHECK(t.Connect(virt) == 0);
  }
}

TEST_CASE("A fifth pad is connected but past the last player", "[input][slots]") {
  Table t;
  for (uint64_t i = 1; i <= 4; ++i) CHECK(t.Connect(Pad(i, "g" + std::to_string(i), "s" + std::to_string(i))) == i - 1);
  CHECK(t.Connect(Pad(5, "g5", "s5")) >= 4);
}

TEST_CASE("With remembering off a returning pad is a new pad", "[input][slots]") {
  Table t;
  t.table.SetRememberDisconnected(false);
  CHECK(t.Connect(Pad(1, "g1", "s1")) == 0);
  CHECK(t.Connect(Pad(2, "g2", "s2")) == 1);
  t.Disconnect(1);
  CHECK(t.table.Reservations().empty());
  CHECK(t.Connect(Pad(3, "g1", "s1")) == 0);  // lowest free, not by memory
  t.Disconnect(1 + 1);                         // player 2 leaves
  CHECK(t.Connect(Pad(4, "g4", "s4")) == 1);
}

TEST_CASE("Pads placed together at launch go in ascending port order", "[input][slots]") {
  DeviceInfo b = Pad(1, "b");
  b.location = "14300000";
  DeviceInfo a = Pad(2, "a");
  a.location = "14200000";
  DeviceInfo no_port = Pad(3, "n", "serial");
  const auto order = PlacementOrder({b, no_port, a});
  REQUIRE(order.size() == 3);
  // The two port pads swap places; the pad without a port keeps its position.
  CHECK(order[0] == 2);
  CHECK(order[1] == 1);
  CHECK(order[2] == 0);
}
