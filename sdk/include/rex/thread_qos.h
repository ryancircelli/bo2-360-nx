/**
 * Runtime switches for thread-placement experiments (all default off, flippable mid-run by the A/B tool).
 * Kept out of rex/thread.h: that header is in the recompiled game's precompiled header.
 */
#pragma once

#include <cstdint>
#include <string_view>

#if defined(__GNUC__)
#define REX_QOS_EXPORT __attribute__((visibility("default")))
#else
#define REX_QOS_EXPORT
#endif

namespace rex::thread {

// thread_qos_critical: Main XThread / GPU Commands stay at user-interactive, other guest threads drop to user-initiated.
REX_QOS_EXPORT bool QosCriticalRequested();
REX_QOS_EXPORT void RequestQosCritical(bool on);
// server_yield_sleep: the guest thread named "Server" sleeps 200 us instead of sched-yield-spinning on a zero delay.
REX_QOS_EXPORT bool ServerYieldSleepRequested();
REX_QOS_EXPORT void RequestServerYieldSleep(bool on);
// shader_pool_cap: shader-compile pool threads run at utility QoS and at most 2 compile at once while frames are swapping.
REX_QOS_EXPORT bool ShaderPoolCapRequested();
REX_QOS_EXPORT void RequestShaderPoolCap(bool on);

// Bumped whenever thread_qos_critical flips, so each thread re-applies its own class at its next wait.
REX_QOS_EXPORT uint32_t QosGeneration();
// Applies (calling thread only) the class for a guest thread of this name. No-op off Apple.
REX_QOS_EXPORT void ApplyGuestThreadQos(std::string_view thread_name);
// Cheap per-wait check: re-applies the calling thread's class if the switch flipped since it last did (or first call).
REX_QOS_EXPORT void ReapplyQosIfFlipped();
// True if the calling thread is the guest thread named "Server" (name read once from the OS thread name).
REX_QOS_EXPORT bool CurrentThreadIsServer();
// Frame-in-flight signal: stamped by every guest swap; true if one happened in the last 100 ms.
REX_QOS_EXPORT void NoteFrameSwap();
REX_QOS_EXPORT bool FrameInFlight();
}  // namespace rex::thread
