/**
 * Xenia .xtr trace writer. See trace_writer.h for attribution.
 */

#include "trace_writer.h"

#include <cstring>

#include <zlib.h>

namespace rex::graphics::trace {

bool TraceWriter::Open(const std::filesystem::path& path, uint32_t title_id) {
  Close();
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  file_ = std::fopen(path.string().c_str(), "wb");
  if (!file_) {
    return false;
  }
  path_ = path;
  bytes_written_ = 0;
  // Large buffer: a frame is typically tens to hundreds of MB.
  std::setvbuf(file_, nullptr, _IOFBF, 8 * 1024 * 1024);

  TraceHeader header = {};
  header.version = kTraceFormatVersion;
  // Xenia stores the 40 char build SHA; ReXGlue stores a tag instead.
  static const char kTag[] = "rexglue-gputest";
  std::memset(header.build_commit_sha, '0', sizeof(header.build_commit_sha));
  std::memcpy(header.build_commit_sha, kTag, sizeof(kTag) - 1);
  header.title_id = title_id;
  Write(&header, sizeof(header));
  return true;
}

void TraceWriter::Flush() {
  if (file_) {
    std::fflush(file_);
  }
}

void TraceWriter::Close() {
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

void TraceWriter::Write(const void* data, size_t size) {
  if (!file_ || !size) {
    return;
  }
  std::fwrite(data, 1, size, file_);
  bytes_written_ += size;
}

void TraceWriter::WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count) {
  PrimaryBufferStartCommand cmd = {TraceCommandType::kPrimaryBufferStart, base_ptr, 0};
  (void)count;
  Write(&cmd, sizeof(cmd));
}

void TraceWriter::WritePrimaryBufferEnd() {
  PrimaryBufferEndCommand cmd = {TraceCommandType::kPrimaryBufferEnd};
  Write(&cmd, sizeof(cmd));
}

void TraceWriter::WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count) {
  IndirectBufferStartCommand cmd = {TraceCommandType::kIndirectBufferStart, base_ptr, 0};
  (void)count;
  Write(&cmd, sizeof(cmd));
}

void TraceWriter::WriteIndirectBufferEnd() {
  IndirectBufferEndCommand cmd = {TraceCommandType::kIndirectBufferEnd};
  Write(&cmd, sizeof(cmd));
}

void TraceWriter::WritePacketStart(uint32_t base_ptr, const void* data, uint32_t count) {
  PacketStartCommand cmd = {TraceCommandType::kPacketStart, base_ptr, count};
  Write(&cmd, sizeof(cmd));
  Write(data, size_t(count) * sizeof(uint32_t));
}

void TraceWriter::WritePacketEnd() {
  PacketEndCommand cmd = {TraceCommandType::kPacketEnd};
  Write(&cmd, sizeof(cmd));
}

void TraceWriter::WriteMemoryRead(uint32_t base_ptr, const void* data, uint32_t length) {
  MemoryCommand cmd = {};
  cmd.type = TraceCommandType::kMemoryRead;
  cmd.base_ptr = base_ptr;
  cmd.encoding_format = MemoryEncodingFormat::kNone;
  cmd.encoded_length = length;
  cmd.decoded_length = length;
  memory_raw_bytes_ += length;
  if (compress_ && length >= 256) {
    uLongf bound = compressBound(length);
    if (compress_buffer_.size() < bound) {
      compress_buffer_.resize(bound);
    }
    uLongf out_length = bound;
    if (compress2(compress_buffer_.data(), &out_length, static_cast<const Bytef*>(data), length,
                  1) == Z_OK &&
        out_length < length) {
      cmd.encoding_format = MemoryEncodingFormat::kZlib;
      cmd.encoded_length = uint32_t(out_length);
      Write(&cmd, sizeof(cmd));
      Write(compress_buffer_.data(), out_length);
      return;
    }
  }
  Write(&cmd, sizeof(cmd));
  Write(data, length);
}

void TraceWriter::WriteEvent(EventCommand::Type event_type) {
  EventCommand cmd = {TraceCommandType::kEvent, event_type};
  Write(&cmd, sizeof(cmd));
}

void TraceWriter::WriteRegisters(uint32_t first_register, const uint32_t* values,
                                 uint32_t register_count, bool execute_callbacks) {
  RegistersCommand cmd = {};
  cmd.type = TraceCommandType::kRegisters;
  cmd.first_register = first_register;
  cmd.register_count = register_count;
  cmd.execute_callbacks = execute_callbacks;
  cmd.encoding_format = MemoryEncodingFormat::kNone;
  cmd.encoded_length = register_count * uint32_t(sizeof(uint32_t));
  Write(&cmd, sizeof(cmd));
  Write(values, cmd.encoded_length);
}

void TraceWriter::WriteGammaRamp(const void* data, uint32_t length, uint8_t rw_component) {
  GammaRampCommand cmd = {};
  cmd.type = TraceCommandType::kGammaRamp;
  cmd.rw_component = rw_component;
  cmd.encoding_format = MemoryEncodingFormat::kNone;
  cmd.encoded_length = length;
  Write(&cmd, sizeof(cmd));
  Write(data, length);
}

}  // namespace rex::graphics::trace
