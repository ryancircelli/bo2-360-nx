#pragma once

#if defined(__APPLE__)
namespace rex::graphics::metal {
// Requested encode-thread state, initialized from metal_encode_thread (default false).
// CP latches requests after ending/draining the frame's encoders and committing its command buffer.
__attribute__((visibility("default"))) bool EncodeThreadRequested();
__attribute__((visibility("default"))) void RequestEncodeThread(bool on);
// Type-0 sequential register batching: same request/latch protocol, default false.
__attribute__((visibility("default"))) bool Type0BatchingRequested();
__attribute__((visibility("default"))) void RequestType0Batching(bool on);
// Master switch for the cmdcost optimizations (draw/const fast path, shared-memory
// upload fix, encode thread + xpass). Same request/latch protocol, default false.
__attribute__((visibility("default"))) bool CmdcostAllRequested();
__attribute__((visibility("default"))) void RequestCmdcostAll(bool on);
// Encode-thread no-sync: tile resolves and draw-pass transfers are recorded on
// the encode thread instead of draining it (metal_encode_nosync, env
// REX_METAL_ENCODE_NOSYNC=1). Same request/latch protocol, default false.
__attribute__((visibility("default"))) bool EncodeNosyncRequested();
__attribute__((visibility("default"))) void RequestEncodeNosync(bool on);
// Command-thread state early-outs (default false); same request/latch protocol.
__attribute__((visibility("default"))) bool RegEarlyOutRequested();
__attribute__((visibility("default"))) void RequestRegEarlyOut(bool on);
__attribute__((visibility("default"))) bool BindlessCacheRequested();
__attribute__((visibility("default"))) void RequestBindlessCache(bool on);
__attribute__((visibility("default"))) bool DrawConstsDirtyRequested();
__attribute__((visibility("default"))) void RequestDrawConstsDirty(bool on);
// [mct] Cross-pass encode thread: EnsureCommandBuffer stops draining the encode
// thread at pass boundaries (encoder creation, events and commit still do).
__attribute__((visibility("default"))) bool XpassLazyBarrierRequested();
__attribute__((visibility("default"))) void RequestXpassLazyBarrier(bool on);
// [mct] Encode-thread hand-off spins (metal_encode_spin_us) before sleeping,
// on the command thread's drain and the worker's idle wait.
__attribute__((visibility("default"))) bool EncodeSpinRequested();
__attribute__((visibility("default"))) void RequestEncodeSpin(bool on);
// [mct] Type-0 batching split: batch only whole shader-constant ranges.
__attribute__((visibility("default"))) bool Type0BatchConstsOnlyRequested();
__attribute__((visibility("default"))) void RequestType0BatchConstsOnly(bool on);
// [mct] Diagnostic per-draw command-thread pad (metal_cmd_pad_ns).
// [mct] Command-thread idle handling (metal_idle_spin, metal_idle_defer_submit).
__attribute__((visibility("default"))) bool IdleSpinRequested();
__attribute__((visibility("default"))) void RequestIdleSpin(bool on);
__attribute__((visibility("default"))) bool IdleDeferSubmitRequested();
__attribute__((visibility("default"))) void RequestIdleDeferSubmit(bool on);
__attribute__((visibility("default"))) bool CmdPadRequested();
__attribute__((visibility("default"))) void RequestCmdPad(bool on);
}  // namespace rex::graphics::metal
#endif
