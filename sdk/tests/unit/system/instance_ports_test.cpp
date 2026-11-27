#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

#include <rex/system/instance_ports.h>

using namespace rex::system::instance_ports;

TEST_CASE("no instance: plain offset", "[instance_ports]") {
  CHECK(Host(1000, -1, 30000, 1024) == 31000);
  CHECK(Host(3074, -1, 30000, 1024) == 3074);
  CHECK(Host(5000, -1, 30000, 1024) == 5000);
  CHECK(Host(1000, -1, 0, 1024) == 1000);
  CHECK(Guest(31000, false, 30000, 1024) == 1000);
  CHECK(Guest(5000, false, 30000, 1024) == 5000);
}

TEST_CASE("instances never collide and round-trip", "[instance_ports]") {
  for (int i = 0; i < kMaxInstances; ++i) {
    for (uint16_t g : {uint16_t(1), uint16_t(88), uint16_t(1000), uint16_t(1001), uint16_t(1023), kSystemLinkPort}) {
      const uint16_t h = Host(g, i, 30000, 1024);
      int who = -1;
      CHECK(Guest(h, true, 30000, 1024, &who) == g);
      CHECK(who == i);
      for (int j = 0; j < i; ++j) CHECK(Host(g, j, 30000, 1024) != h);
    }
  }
}

TEST_CASE("unmoved and overflowing ports stay", "[instance_ports]") {
  CHECK(Host(5000, 3, 30000, 1024) == 5000);
  CHECK(Host(1000, 15, 60000, 1024) == 1000);
  CHECK(!Moved(0));
}
