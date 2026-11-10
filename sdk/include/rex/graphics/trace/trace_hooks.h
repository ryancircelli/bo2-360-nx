/**
 * ReXGlue GPU frame trace hooks.
 *
 * The command processor calls these at well defined points while it parses
 * the PM4 stream. FrameCapture implements them to write Xenia style .xtr
 * traces; FrameReplay implements them to drive per-draw readback while a
 * trace is being played back. When no hooks are installed the command
 * processor only pays for a null pointer check per packet.
 */

#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {
class CommandProcessor;
}  // namespace rex::graphics

namespace rex::graphics::trace {

struct DrawEvent {
  xenos::PrimitiveType prim_type = xenos::PrimitiveType::kNone;
  xenos::SourceSelect source_select = xenos::SourceSelect::kAutoIndex;
  uint32_t index_count = 0;
  bool is_indexed = false;
  xenos::IndexFormat index_format = xenos::IndexFormat::kInt16;
  xenos::Endian index_endianness = xenos::Endian::kNone;
  uint32_t index_guest_base = 0;
  uint32_t index_length_bytes = 0;
  // Set for OnDrawEnd: whether the backend accepted the draw.
  bool succeeded = false;
};

class TraceHooks {
 public:
  virtual ~TraceHooks() = default;

  // True while a trace is being played back. The command processor then skips
  // WAIT_REG_MEM spins (the recorded values already satisfied them).
  virtual bool is_replaying() const { return false; }

  // Cheap check used before the per-packet hooks.
  virtual bool is_recording() const { return false; }

  virtual void OnPrimaryBufferStart(uint32_t /*base_ptr*/, uint32_t /*count*/) {}
  virtual void OnPrimaryBufferEnd() {}
  virtual void OnIndirectBufferStart(uint32_t /*base_ptr*/, uint32_t /*count*/) {}
  virtual void OnIndirectBufferEnd() {}

  // host_ptr points at the packet header inside a ring of ring_capacity bytes
  // starting at ring_base (packets may wrap). dword_count includes the header.
  virtual void OnPacketStart(const uint8_t* /*ring_base*/, uint32_t /*ring_capacity*/,
                             uint32_t /*read_offset*/, uint32_t /*dword_count*/) {}
  virtual void OnPacketEnd() {}

  // Physical guest memory the GPU is about to read (constants, ucode, polled
  // words). Vertex, index and texture data are collected in OnDrawEnd.
  virtual void OnMemoryRead(uint32_t /*physical_address*/, uint32_t /*length*/) {}

  virtual void OnDrawEnd(CommandProcessor& /*cp*/, const DrawEvent& /*draw*/) {}

  // After XE_SWAP was processed (counter already incremented).
  virtual void OnSwap(CommandProcessor& /*cp*/, uint32_t /*frontbuffer_ptr*/,
                      uint32_t /*width*/, uint32_t /*height*/) {}
};

}  // namespace rex::graphics::trace
