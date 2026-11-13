// Start-up and shutdown hooks that the app calls at fixed points of a run (bo2zm_register.cpp, bo2zm_benchmark.cpp).
//
// A build for development consoles fills them with its PC link (log uploads, new builds, the benchmark watchdog). The
// standard build compiles bo2zm_lifecycle.cpp instead, which keeps only what every console needs: the console stays
// awake through a benchmark, and the first constructor hands over to the campaign program when a launch for it is
// pending (bo2zm_relaunch.h, HandOverEarly).

#pragma once

namespace bo2zm::lifecycle {

// OnPreSetup, before the input wrappers or any diagnostic cache exists. False stops the start.
bool Prepare();

// OnPreSetup, once logging exists.
void OnBoot();

// The benchmark route has ended and its summary is in the log.
void OnBenchmarkDone();

// OnShutdown: waits for any work these hooks started.
void Stop();

}  // namespace bo2zm::lifecycle
