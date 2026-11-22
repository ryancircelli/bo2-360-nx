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

TEST_CASE("Model fallback never takes another serial's reservation", "[input][slots]") {
  Table t;
  CHECK(t.Connect(Pad(1, "usb-guid", "serial-a")) == 0);
  t.Disconnect(1);

  CHECK(t.Connect(Pad(2, "bluetooth-guid", "serial-b")) == 1);
  CHECK(t.Connect(Pad(3, "usb-guid", "serial-a")) == 0);
}
