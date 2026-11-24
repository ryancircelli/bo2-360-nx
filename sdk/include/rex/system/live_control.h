/**
 * @file        system/live_control.h
 * @brief       Commands for a running test game: cvars, console commands, patch swaps, time scale, save points
 *
 * The Metal test control channel (REX_METAL_TEST_CONTROL_DIR, rex/ui/metal/metal_test_control.h) hands every
 * request it does not know itself to LiveControlCommand, so `runq live exec` can change a game while it runs:
 *
 *   cvar NAME [VALUE]        read, or set a hot-reload cvar (init-only cvars are refused)
 *   cmd NAME [ARGS]          run a console command (a cvar of type Command)
 *   patch PATH               load a patch library (rex/system/patch.h); waits for the next frame to bind it
 *   patch off                every slot back to the original recompiled body
 *   patch status             committed version, slot count, patched slots
 *   slots [FILTER]           patchable slot names (first 200 matching)
 *   timescale [X]            read, or set guest time scale live (0.1..16; 1 = real time)
 *   savepoint save PATH      write guest memory at the next frame (rex/system/savepoint.h)
 *   savepoint load PATH      not available yet (the savepoint-rewind work plugs in here)
 *
 * Replies start with "ok" or "error". Unknown commands return an empty string.
 */

#pragma once

#include <string>

namespace rex::system {

std::string LiveControlCommand(const std::string& command, const std::string& args);

}  // namespace rex::system
