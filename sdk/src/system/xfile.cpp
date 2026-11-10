/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/thread/mutex.h>

#include <span>
#include <chrono>
#include <mutex>
#include <cstdio>

namespace rex::system {

XFile::XFile(KernelState* kernel_state, rex::filesystem::File* file, bool synchronous)
    : XObject(kernel_state, kObjectType), file_(file), is_synchronous_(synchronous) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::XFile() : XObject(kObjectType) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // TODO(benvanik): signal that the file is closing?
  async_event_->Set();
  file_->Destroy();
}

uint64_t XFile::position() const {
  std::lock_guard<std::mutex> lock(file_lock_);
  return position_;
}

void XFile::set_position(uint64_t value) {
  std::lock_guard<std::mutex> lock(file_lock_);
  position_ = value;
}

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                               const std::string_view file_name, bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  rex::filesystem::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(rex::string::utf8_find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() > end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

namespace {
// [file-read-ring] Recent file reads into guest physical memory, for the Metal
// texture content check: a mismatching texture range is matched against them.
struct FileReadRecord {
  uint32_t phys = 0, length = 0;
  uint64_t time_us = 0;
  bool triggered = false;
  char kind = 'f';
  char path[56] = {};
};
constexpr size_t kFileReadRingSize = 8192;
std::mutex file_read_ring_mutex;
FileReadRecord file_read_ring[kFileReadRingSize];
uint64_t file_read_ring_count = 0;
uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace

void RecordGuestWrite(char kind, uint32_t phys, uint32_t length, bool triggered,
                      const std::string& path) {
  std::lock_guard<std::mutex> lock(file_read_ring_mutex);
  FileReadRecord& r = file_read_ring[file_read_ring_count++ % kFileReadRingSize];
  r.kind = kind;
  r.phys = phys;
  r.length = length;
  r.time_us = NowUs();
  r.triggered = triggered;
  size_t n = path.size(), keep = sizeof(r.path) - 1;
  std::snprintf(r.path, sizeof(r.path), "%s", path.c_str() + (n > keep ? n - keep : 0));
}

std::string DescribeGuestWritesOverlapping(uint32_t phys_start, uint32_t length) {
  std::lock_guard<std::mutex> lock(file_read_ring_mutex);
  uint64_t first = file_read_ring_count > kFileReadRingSize ? file_read_ring_count - kFileReadRingSize : 0;
  uint32_t hits[2] = {};
  const FileReadRecord* last[2] = {};
  for (uint64_t i = first; i < file_read_ring_count; ++i) {
    const FileReadRecord& r = file_read_ring[i % kFileReadRingSize];
    if (r.phys < phys_start + length && phys_start < r.phys + r.length) {
      int k = r.kind == 'm' ? 1 : 0;
      ++hits[k];
      last[k] = &r;
    }
  }
  std::string out = "writer " + std::string(last[0] && last[1] ? "file read + memexport"
                                            : last[0]           ? "file read"
                                            : last[1]           ? "memexport"
                                                                : "neither");
  const uint64_t now = NowUs();
  for (int k = 0; k < 2; ++k) {
    if (!last[k]) continue;
    char buf[200];
    std::snprintf(buf, sizeof(buf), ", %u %s, last %08X+%u %.1f s ago triggered %d %s", hits[k],
                  k ? "memexports" : "file reads", last[k]->phys, last[k]->length,
                  double(now - last[k]->time_us) / 1e6, int(last[k]->triggered), last[k]->path);
    out += buf;
  }
  out += " (ring " + std::to_string(file_read_ring_count - first) + ")";
  return out;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                     uint32_t* out_bytes_read, uint32_t apc_context, bool notify_completion) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return ReadInternal(buffer_guest_address, buffer_length, byte_offset, out_bytes_read, apc_context,
                      notify_completion);
}

X_STATUS XFile::ReadInternal(uint32_t buffer_guest_address, uint32_t buffer_length,
                             uint64_t byte_offset, uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion) {
  if (byte_offset == uint64_t(-1)) {
    // Read from current position.
    byte_offset = position_;
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      uint32_t buffer_guest_high_address = buffer_guest_address + buffer_length - 1;
      rex::memory::BaseHeap* buffer_start_heap = memory()->LookupHeap(buffer_guest_address);
      const rex::memory::BaseHeap* buffer_end_heap =
          memory()->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == memory::HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        rex::memory::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical
                ? static_cast<rex::memory::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap && buffer_physical_heap->QueryRangeAccess(
                                        buffer_guest_address, buffer_guest_high_address) !=
                                        memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          result = file_->ReadSync(
              std::span<uint8_t>(
                  buffer_physical_heap
                      ? memory()->TranslatePhysical(
                            buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))
                      : memory()->TranslateVirtual(buffer_guest_address),
                  buffer_length),
              size_t(byte_offset), &bytes_read);
          if (XSUCCEEDED(result)) {
            if (buffer_physical_heap) {
              bool triggered = buffer_physical_heap->TriggerCallbacks(
                  rex::thread::global_critical_region::AcquireDirect(), buffer_guest_address,
                  buffer_length, true, true);
              RecordGuestWrite('f',
                               buffer_physical_heap->GetPhysicalAddress(buffer_guest_address),
                               buffer_length, triggered, path());
            }

            if (byte_offset) {
              position_ = byte_offset;
            }
            position_ += bytes_read;
          }
        }
      }
    }
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  if (notify_completion) {
    XIOCompletion::IONotification notify;
    notify.apc_context = apc_context;
    notify.num_bytes = uint32_t(bytes_read);
    notify.status = result;

    NotifyIOCompletionPorts(notify);

    async_event_->Set();
  }

  return result;
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length, uint64_t byte_offset,
                            uint32_t* out_bytes_read, uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;

  // segments points to an array of buffer pointers of type
  // "FILE_SEGMENT_ELEMENT", but they can just be treated as normal pointers
  rex::be<uint32_t>* segments =
      reinterpret_cast<rex::be<uint32_t>*>(memory()->TranslateVirtual(segments_guest_address));

  // TODO: not sure if this is meant to change depending on buffer address?
  // (only game seen using this always seems to use 4096-byte buffers)
  uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  while (read_remain) {
    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    uint32_t bytes_read = 0;
    result = ReadInternal(
        read_buffer, read_length,
        byte_offset
            ? ((byte_offset != -1 && byte_offset != -2) ? byte_offset + read_total : byte_offset)
            : -1,
        &bytes_read, apc_context, false);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(read_total);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  async_event_->Set();

  return result;
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                      uint32_t* out_bytes_written, uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_;
  }

  size_t bytes_written = 0;
  X_STATUS result = file_->WriteSync(
      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),
      size_t(byte_offset), &bytes_written);
  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(bytes_written);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  async_event_->Set();
  return result;
}

X_STATUS XFile::SetLength(size_t length) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return file_->SetLength(length);
}

X_STATUS XFile::Rename(const std::filesystem::path& file_path) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return entry()->Rename(file_path);
}

void XFile::RegisterIOCompletionPort(uint32_t key, object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  completion_ports_.push_back({key, port});
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end(); it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XFile {:08X} ({})", handle(), file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(file_->entry()->absolute_path());
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>((file_->entry()->attributes() & rex::filesystem::kFileAttributeDirectory) !=
                      0);
  stream->Write<bool>(is_synchronous_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();

  REXSYS_DEBUG("XFile {:08X} ({})", file->handle(), abs_path);

  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(nullptr, abs_path,
                                                   rex::filesystem::FileDisposition::kOpen, access,
                                                   is_directory, false, &vfs_file, &action);
  if (XFAILED(res)) {
    REXSYS_ERROR("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;

  return object_ref<XFile>(file);
}

void XFile::NotifyIOCompletionPorts(XIOCompletion::IONotification& notification) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto port : completion_ports_) {
    notification.key_context = port.first;
    port.second->QueueNotification(notification);
  }
}

}  // namespace rex::system
