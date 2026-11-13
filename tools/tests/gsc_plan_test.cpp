// Authored metadata exercises composition; no game bytecode is used.
#include "../../games/bo2/src/bo2zm_gsc_plan.h"
#include <cassert>
#include <iostream>
using namespace bo2zm::gsc;
Object Item(std::string name, uint8_t value = 0) {
  return {{std::move(name)}, std::vector<uint8_t>(64, value), {}};
}
template<class F> void Refuse(F fn) {
  bool refused = false;
  try { fn(); } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
}
int main() {
  Component menu, bots;
  menu.objects = {Item("maps/mp/menu.gsc")};
  menu.options.entry = "maps/mp/menu.gsc";
  bots.objects = {Item("maps/mp/bots.gsc"), Item("maps/mp/origins.gsc")};
  bots.options.entry = "maps/mp/bots.gsc";
  bots.options.map_entries = {{"zm_tomb", "maps/mp/origins.gsc"}};
  bots.options.replace = "maps/mp/zombies/_zm_utility.gsc::tracker=>maps/mp/bots.gsc::tracker";
  Plan plan;
  AddComponent(plan, menu, "zm_transit");
  assert(plan.objects.size() == 1 && plan.entries.size() == 1); // Bots off.
  AddComponent(plan, bots, "zm_transit");
  assert(plan.objects.size() == 3 && plan.entries.back() == bots.options.entry);
  AddComponent(plan, bots, "zm_transit");
  assert(plan.objects.size() == 3 && plan.entries.size() == 2);
  Plan origins;
  AddComponent(origins, bots, "zm_tomb");
  assert(origins.entries.front() == "maps/mp/origins.gsc");
  auto conflict = bots;
  conflict.objects.front().bytes[0] = 1;
  Refuse([&] { AddComponent(plan, conflict, "zm_transit"); });
  assert(plan.objects.size() == 3 && plan.objects[1].bytes[0] == 0);
  conflict = menu;
  conflict.objects.push_back(Item("maps/mp/zombies/_zm_utility.gsc"));
  Plan stock_override;
  AddComponent(stock_override, conflict, "zm_transit");
  Refuse([&] { AddComponent(stock_override, bots, "zm_transit"); });
  assert(stock_override.objects.size() == 2 && stock_override.replace.empty());
  conflict = bots;
  conflict.options.replace = "maps/mp/other.gsc::tracker=>maps/mp/bots.gsc::tracker";
  Refuse([&] { AddComponent(plan, conflict, "zm_transit"); });
  assert(plan.replace == bots.options.replace);
  conflict = menu;
  conflict.options.maps = "zm_tomb";
  Plan restricted;
  AddComponent(restricted, conflict, "zm_transit");
  assert(restricted.objects.empty() && restricted.entries.empty());
  Component budget;
  budget.objects = {Item("maps/mp/huge.gsc")};
  budget.objects.front().bytes.resize(4 * 1024 * 1024);
  Refuse([&] { AddComponent(plan, budget, "zm_transit"); });
  assert(plan.objects.size() == 3);
  assert(SameComponent(bots, bots) && !SameComponent(menu, bots));
  conflict = bots;
  std::reverse(conflict.objects.begin(), conflict.objects.end());
  assert(SameComponent(bots, conflict));
  conflict.options.map_entries.clear();
  assert(!SameComponent(bots, conflict));
  std::cout << "script plan fixtures passed\n";
}
