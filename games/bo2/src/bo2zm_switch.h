#pragma once

namespace bo2zm {

// Starts the Horizon services the game needs (sockets). No-op off the Switch.
// Call before the runtime is set up (OnPreSetup).
void InitSwitchServices();

// The build identifier every log names (BO2ZM_BUILD_ID: build_switch.sh / build_windows.sh pass the time).
const char* BuildId();

// CPU cores this process may use: 3 from hbmenu or a normal forwarder, 4 from a forwarder that allows core 3
// (sdk/src/ui/switch_core3.cpp). 0 off the Switch.
int ProcessCores();

// The CPU CORES setting (bo2zm_cpu_cores, Extras > EXPERIMENTAL): 3 or 4. Setting it moves the threads at once when the
// launch has four cores, and changes nothing otherwise.
int CpuCoresSetting();
void SetCpuCoresSetting(int cores);
// The cores the game uses right now: 4 only with the setting at 4 and a 4-core launch. 0 off the Switch.
int CoresInUse();
// The failsafe switched the fourth core off (our threads held core 3), until CPU CORES switches it on again.
bool CoresSwitchedOff();

// The screen's refresh signal (vi's vsync event), for VSYNC's pacing (bo2zm_extras.cpp PaceFrame). The first call asks
// vi for it (once: vi gives it once per display); false off the Switch or when vi refused it. Main thread only.
bool HasRefreshSignal();
// Waits for the screen's next refresh, at most timeout_ms; false on a timeout or without the signal.
bool WaitForNextRefresh(int timeout_ms);

}  // namespace bo2zm
