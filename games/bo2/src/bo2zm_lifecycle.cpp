// The start-up hooks (bo2zm_lifecycle.h) and the home screen's update check (bo2zm_updates.h) for the standard build,
// which has no update server: the CHECK FOR UPDATES button is left out of the menus (games/bo2/CMakeLists.txt), and the
// update functions do nothing.
//
// The early hand-over to the campaign program runs from a constructor of the first priority a program may use (101),
// while the main thread is still the only one: on the Switch a new NRO is started by returning to hbloader, which
// loads it into the same process, so any thread started before that keeps running there (bo2zm_relaunch.cpp).

#include "bo2zm_lifecycle.h"

#include <string_view>

#include <rex/ppc.h>

#include "bo2zm_relaunch.h"
#include "bo2zm_updates.h"


namespace bo2zm::lifecycle {

bool Prepare() { return true; }

void OnBoot() {
}

void OnBenchmarkDone() {}

void Stop() {}

}  // namespace bo2zm::lifecycle

namespace bo2zm::updates {
void Command(std::string_view) {}
void EveryFrame(PPCContext&, uint8_t*) {}
void Stop() {}
}  // namespace bo2zm::updates

