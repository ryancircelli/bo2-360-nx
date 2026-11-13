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
// Command-thread state early-outs (default false); same request/latch protocol.
__attribute__((visibility("default"))) bool RegEarlyOutRequested();
__attribute__((visibility("default"))) void RequestRegEarlyOut(bool on);
__attribute__((visibility("default"))) bool BindlessCacheRequested();
__attribute__((visibility("default"))) void RequestBindlessCache(bool on);
__attribute__((visibility("default"))) bool DrawConstsDirtyRequested();
__attribute__((visibility("default"))) void RequestDrawConstsDirty(bool on);
}  // namespace rex::graphics::metal
#endif
