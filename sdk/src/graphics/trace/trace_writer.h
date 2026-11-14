/**
 * Xenia .xtr trace writer.
 *
 * Structure and command layout follow xenia-project/xenia
 * src/xenia/gpu/trace_writer.cc (BSD-3, Copyright Ben Vanik). Data is always
 * written uncompressed (MemoryEncodingFormat::kNone), which every Xenia
 * derived reader accepts.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <rex/graphics/trace/trace_protocol.h>

namespace rex::graphics::trace {

class TraceWriter {
 public:
  TraceWriter() = default;
  ~TraceWriter() { Close(); }
  TraceWriter(const TraceWriter&) = delete;
  TraceWriter& operator=(const TraceWriter&) = delete;

  bool is_open() const { return file_ != nullptr; }
  const std::filesystem::path& path() const { return path_; }
  uint64_t bytes_written() const { return bytes_written_; }

  bool Open(const std::filesystem::path& path, uint32_t title_id);
  void Flush();
  void Close();

  void WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count);
  void WritePrimaryBufferEnd();
  void WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count);
  void WriteIndirectBufferEnd();
  // data holds count dwords exactly as they sit in guest memory.
  void WritePacketStart(uint32_t base_ptr, const void* data, uint32_t count);
  void WritePacketEnd();
  void WriteMemoryRead(uint32_t base_ptr, const void* data, uint32_t length);
  void WriteEvent(EventCommand::Type event_type);
  void WriteRegisters(uint32_t first_register, const uint32_t* values, uint32_t register_count,
                      bool execute_callbacks);
  void WriteGammaRamp(const void* data, uint32_t length, uint8_t rw_component);

 private:
  void Write(const void* data, size_t size);

  std::FILE* file_ = nullptr;
  std::filesystem::path path_;
  uint64_t bytes_written_ = 0;
};

}  // namespace rex::graphics::trace
