#pragma once

#include <filesystem>

#include "bo2zm_launch_options.h"

namespace bo2zm {

// Hands the launch data of a pending relaunch (written just before the
// previous process restarted itself) to XAM, so the game reads it with
// XamLoaderGetLaunchData as after a console title reboot. Call once the
// runtime and kernel exist and before the game starts (OnPostSetup).
void ApplyPendingRelaunch(const std::filesystem::path& user_dir);

// Whether Campaign is installed: its files, and its program beside this one (bo2zm_campaign on).
bool CampaignAvailable();

// The installed modes (bo2zm_launch_options.h), read once the runtime knows the game folder. The campaign program
// has its own (bo2cp_relaunch.cpp).
const launch::InstalledModes& ModesInstalled();

// Switch, from the first constructor (bo2zm_lifecycle.h), while the main thread is the only one: chain-loads the
// campaign program for an explicit pending launch, the campaign test loop, or a saved Campaign default.
// Elsewhere nothing.
void HandOverEarly();

// Worker-safe request only. Pump RestartForScriptChanges from main EveryFrame.
void RequestRestart();
// Handles a queued request on the main thread through the existing relaunch path.
// Returns false when idle or if preserving launch data failed; success exits.
bool RestartForScriptChanges();

}  // namespace bo2zm
