/**
 * Scheduling class for the threads that produce sound.
 *
 * Kept out of rex/thread.h on purpose: that header is in the recompiled game's precompiled header, and a declaration
 * added there rebuilds all of the recompiled code.
 */
#pragma once

#include <string>

namespace rex::thread {

class Thread;

// Puts a thread (null = the calling one) in the scheduling class for real-time audio work (the threads that produce
// each sound block before the device asks for it), as the audio_thread_policy cvar says. A no-op where the platform
// has no such class or the policy is "off". Returns what was done, for the log.
std::string SetThreadAudioPriority(Thread* thread = nullptr);

}  // namespace rex::thread
