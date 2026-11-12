#pragma once

#if defined(__APPLE__)
namespace rex::graphics::metal {
// Requested encode-thread state, initialized from metal_encode_thread (default false).
// CP latches requests after ending/draining the frame's encoders and committing its command buffer.
__attribute__((visibility("default"))) bool EncodeThreadRequested();
__attribute__((visibility("default"))) void RequestEncodeThread(bool on);
}  // namespace rex::graphics::metal
#endif
