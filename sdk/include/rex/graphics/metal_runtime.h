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
// Two-stage command processor (metal_cp_two_stage): same protocol. The CP only
// honours requests when it was armed at startup (cvar/env on, or the benchmark
// factor list names it), because an armed CP keeps a second register file.
__attribute__((visibility("default"))) bool CpTwoStageRequested();
__attribute__((visibility("default"))) void RequestCpTwoStage(bool on);
}  // namespace rex::graphics::metal
#endif
