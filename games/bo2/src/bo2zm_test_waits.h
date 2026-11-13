// Stress test of the host runtime's waits and signals: rex::thread events, semaphores, mutants, PulseEvent,
// WaitMultiple (any and all), alertable waits with user callbacks, and timeouts. It was written for the
// address-arbiter waits of the SDK's threading_posix.cpp (REX_FAST_WAITS, sdk/src/core/threading_posix.cpp).
//
// Two ways to run it:
//   - Linux (WSL): tools/runtime_stress/build.sh builds this file with the SDK's threading code and runs it.
//   - In the app, bo2zm_test_waits = true: runs once at start-up (console or PC) and logs one line per test
//     ("[test_waits] PASS ..." / "FAIL ...") and a summary. Off by default; the start-up takes a few seconds longer.
#pragma once

#include <functional>
#include <string>

namespace bo2zm::test_waits {

// scale multiplies the iteration counts and durations (1.0: about 20 s on a PC). Every wait inside has a timeout,
// so a lost wake-up shows as a FAIL line, not as a hang. Returns true when every test passed.
bool Run(double scale, const std::function<void(const std::string&)>& log);

// The cvar entry point: runs Run() if bo2zm_test_waits is set. Called once from the app's start-up.
void RunIfRequested();

}  // namespace bo2zm::test_waits
