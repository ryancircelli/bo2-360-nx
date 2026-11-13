// Who waits for whom during a stutter.
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// During most frames over 45 ms on the console almost no thread is doing real work: the main thread waits for the
// preparer to hand over the frame, the PM4 ring runs dry or waits for the GPU, and the preparer sleeps inside
// sub_826E8EE8 (COM-style virtual calls we have not identified). Each kind of wait adds its time to a counter here,
// and the "[hitch]" lines in bo2zm_native_targets.cpp print how much of the slow frame each one took. Recording a
// wait costs two clock reads, on a path that is about to sleep anyway.

#pragma once

#include <atomic>
#include <cstdint>

namespace bo2zm::waits {

enum Kind : uint32_t {
  kHandoffExecutor,    // Main XThread waiting for the preparer to hand over the frame (sub_8262D988)
  kHandoffPreparer,  // the preparer waiting for the executor to finish the previous one (sub_8262D988)
  kRoomRing,       // the D3D waiting for the ring to advance (sub_825A5D18)
  kGameMid,        // the game inside sub_826E8EE8
  kRingNoJob,  // the ring thread with no packets to read
  kRingRegMem,      // the ring thread in a WAIT_REG_MEM (waiting for the game to write)
  // The game's side in the stutter frame ("[hitch] game" line).
  kExecutorNoOrders,   // Main XThread with the list open and no new commands (sub_823C83F8)
  kPreparerList,      // the preparer filling the command list (all of sub_82445660: culling and eView)
  kPreparerOut,      // the preparer between two fills: its simulation plus its handoff wait
  kPreparerScenery,  // TreeCull (sub_824C2F48) with its DrawAScenery calls, inside the fill
  kNumTypes
};

inline std::atomic<uint64_t> g_ns[kNumTypes];
inline std::atomic<uint64_t> g_times[kNumTypes];
inline std::atomic<uint32_t> g_vtable_mid{0};    // vtable of the object of the last call to sub_826E8EE8
inline std::atomic<uint32_t> g_caller_mid{0};  // and its return address

// What the ring re-checks of the textures (XXH3 of the guest bytes) and how many checks are postponed by
// the per-frame budget (bo2zm_native_hash_kb_per_frame).
inline std::atomic<uint64_t> g_bytes_hash{0};
inline std::atomic<uint64_t> g_hashes_postponed{0};

// What the ring did during the slow frame. Gameplay stutters usually come from a full ring (the game waits
// 18-54 ms for space) while the texture checks stay under 8 MB, so these counters show where the ring's own time
// went. Only the ring thread writes them.
inline std::atomic<uint64_t> g_draws{0};             // draws read from the ring
inline std::atomic<uint64_t> g_ns_ring_working{0};  // reading and executing packets
inline std::atomic<uint64_t> g_ns_textures{0};         // check, detile and prepare the textures in use
inline std::atomic<uint64_t> g_textures_uploads{0};    // textures that changed and are uploaded again
inline std::atomic<uint64_t> g_bytes_uploaded{0};
inline std::atomic<uint64_t> g_textures_created{0};
// Of that texture time, what goes into the two fingerprints (XXH3 of the guest memory and of the already
// prepared data).
inline std::atomic<uint64_t> g_ns_hash_raw{0};
inline std::atomic<uint64_t> g_ns_hash_data{0};
// How long the ring thread waits for the vertex copy thread (WaitUploads in bo2zm_native_draws.cpp, before each
// submit and before returning the read pointer to the game), and how many times it actually had to wait. Only the
// ring thread writes it.
inline std::atomic<uint64_t> g_ns_waiting_copies{0};
inline std::atomic<uint64_t> g_waits_copies{0};
// What the ring copies itself during those waits, from what the thread had not taken yet, and how many
// copies (bo2zm_native_upload_help). g_ns_waiting_copies is therefore only the wait. Ring thread only.
inline std::atomic<uint64_t> g_ns_helping_copies{0};
inline std::atomic<uint64_t> g_copies_helped{0};
// What the ring spends creating textures (image, pool memory and view; the "textures X ms" figure starts after
// creation and does not include it), how long it waits for the binding thread, and how many textures that thread
// bound (bo2zm_native_texture_bind_thread in bo2zm_native_draws.cpp). Only the ring thread writes these.
inline std::atomic<uint64_t> g_ns_create_textures{0};
inline std::atomic<uint64_t> g_ns_waiting_bindings{0};
inline std::atomic<uint64_t> g_textures_bound_thread{0};
// New textures with the fingerprint, the detiling and the byte order done on the fingerprint thread
// (bo2zm_native_texture_hash_thread in bo2zm_native_draws.cpp): what the ring spends copying guest memory
// (snapshots; counted inside "textures"), how many the thread prepares and in how long, how many the ring
// keeps for itself at submit time and in how long, and how long the ring waits for the thread. Only the ring
// thread writes it (when planning and when collecting).
inline std::atomic<uint64_t> g_ns_hash_snapshot{0};
inline std::atomic<uint64_t> g_textures_hash_thread{0};
inline std::atomic<uint64_t> g_ns_hash_thread{0};
inline std::atomic<uint64_t> g_textures_hash_ring{0};
inline std::atomic<uint64_t> g_ns_hash_ring{0};
inline std::atomic<uint64_t> g_ns_waiting_hashes{0};

inline void Add(Kind type, uint64_t ns) {
  g_ns[type].fetch_add(ns, std::memory_order_relaxed);
  g_times[type].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace bo2zm::waits
