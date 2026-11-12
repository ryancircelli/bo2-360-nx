// bo2zm_benchmark: the same measurement on every build. Once player 0 is in a match, the test pad drives a fixed
// 3-minute route (turning in place, 30 s each way), and at the end the log gets one line with the frame times of that
// window: average and median FPS, percentiles, and the share of frames over 33.3 and 50 ms. Use it with
// bo2zm_test_god so the player survives the whole route.
// With bo2zm_test_spot (MARK LOCATION's line) the route is a test spot instead: player 0 stands still at the marked
// feet, looking the marked way, with no zombies, and a burst of screenshots 4 s in shows what changes from frame to
// frame there (tools/spot_flicker.py on the PC).

#pragma once

#include <cstdint>

namespace bo2zm::benchmark {

// Parse and validate named runtime factors during startup, before guest threads start.
void InitializeFactors();

// bo2zm_benchmark is on.
bool IsActive();
// No benchmark in this start whatever the settings say (after a watchdog restart). Before the match.
void Disable();

// From the game's per-frame hook: whether player 0 has a client (a match is loaded).
void AnnotatePlayer(bool present);

// The route's test pad buttons right now (the test pad's bit layout), 0 outside the route.
uint32_t PathButtons();

// While the route runs: the stick tilts it wants, in percent (move: left stick, look: right stick); false outside it
// (the test pad's own: full move, bo2zm_test_look_speed).
bool PathTilts(int& move_percent, int& look_percent);

// Each frame the game presents (the native renderer's Swap).
void AnnotateFrame();
// How many frames AnnotateFrame has seen since the start (for a frame watchdog).
uint64_t FramesSeen();

// bo2zm_benchmark_ab_s: a 2x2 A/B inside the route. The arm changes every N seconds (bit 0 = factor 1 on, bit 1 =
// factor 2 on), in an order that shifts by one each minute so every arm covers different parts of the route; the
// summary gets one line per arm and one per factor (on against off). -1 outside the route or with the A/B off. What
// "on" means is up to each factor's module (bo2zm_native_recorder_core_b, bo2zm_native_nvk_copy_engine_b).
int AbArm();
// AbArm as of the last frame (set once a frame): for code called too often to read the clock each time.
int AbArmCached();

// bo2zm_benchmark_priority_*_ab: called often by the game threads (the job picker and the Backend's frame wait,
// bo2zm_waits.cpp); the Main and Backend threads set their own priority when their factor wants a change. Cheap when
// the factors are off.
void ApplyOwnPriority();

}  // namespace bo2zm::benchmark
