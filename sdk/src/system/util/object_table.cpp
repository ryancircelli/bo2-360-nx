/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdio>
#include <rex/system/xfile.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xnotifylistener.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

namespace rex::system::util {

// Handle lookups without the global critical region.
//
// Why: LookupObject took the global critical region, one recursive mutex shared with the whole runtime (file
// system, texture cache, shared memory, MMIO...), on every NtWaitForSingleObjectEx, NtPulseEvent and NtSetEvent.
// In a console profile the main thread and the Backend had 2-3 % of their samples in that lock from these
// lookups alone, behind whoever else held it.
//
// How: LookupObject reads the slot with atomics and no lock; everything that changes the table still holds the
// global critical region, as before. A lookup counts itself in lookups_in_flight_ for the few instructions
// between reading the slot and retaining the object. A writer that clears a slot (RemoveHandle,
// PurgeAllObjects, Reset) drops the table's reference only once it has seen lookups_in_flight_ at zero after
// clearing the slot: every lookup that could have read the old pointer has retained it by then, so the object
// cannot be deleted under it (the clear, the count and the slot load are sequentially consistent). Growing the
// table copies it instead of realloc, and frees the old copy the same way. If a lookup does not finish within a
// few microseconds (its thread was suspended in the middle), the writer does not wait: the object goes on
// retired_objects_ and the next writer that sees no lookup running releases it. All of this lives here:
// object_table.h is unchanged (the generated code's precompiled header includes it).
namespace {

std::atomic<uint32_t> lookups_in_flight_{0};
// Objects and old tables waiting for no lookup to be running. Protected by the global critical region.
std::vector<XObject*> retired_objects_;
std::vector<void*> retired_tables_;

template <typename T>
T AtomicLoad(T& field) {
  return std::atomic_ref<T>(field).load(std::memory_order_seq_cst);
}
template <typename T>
void AtomicStore(T& field, T value) {
  std::atomic_ref<T>(field).store(value, std::memory_order_seq_cst);
}

// True once no lookup is running. Bounded: a few microseconds.
bool NoLookupRunning() {
  for (int i = 0; i < 256; ++i) {
    if (lookups_in_flight_.load(std::memory_order_seq_cst) == 0) {
      return true;
    }
    if (i >= 192) {
      std::this_thread::yield();
    }
  }
  return false;
}

// Called with the global critical region held, after the slots of everything retired were cleared.
void ReleaseRetired() {
  if ((retired_objects_.empty() && retired_tables_.empty()) || !NoLookupRunning()) {
    return;
  }
  std::vector<void*> tables;
  tables.swap(retired_tables_);
  for (void* table : tables) {
    free(table);
  }
  // A destructor may close handles and retire more: release from a copy.
  std::vector<XObject*> objects;
  objects.swap(retired_objects_);
  for (XObject* object : objects) {
    object->Release();
  }
}

}  // namespace

ObjectTable::ObjectTable() {}

ObjectTable::~ObjectTable() {
  Reset();
}

struct ObjectTable::RewindState {
  uint32_t last_free_entry = 0;
  std::vector<ObjectTableEntry> entries;
  std::unordered_map<string::string_key_case, X_HANDLE> names;
  std::vector<std::pair<object_ref<XObject>, XObject::RewindState>> objects;
  // Host-side file positions: reads after the capture advance them, and NtReadFile without an
  // explicit offset reads from there.
  std::vector<std::pair<XFile*, uint64_t>> file_positions;
  std::vector<std::pair<XIOCompletion*, XIOCompletion::QueueRewindState>> completions;
  std::vector<std::pair<XNotifyListener*, std::vector<std::pair<XNotificationID, uint32_t>>>> listeners;
};

std::shared_ptr<ObjectTable::RewindState> ObjectTable::CaptureRewindState() {
  auto global_lock = global_critical_region_.Acquire();
  auto state = std::make_shared<RewindState>();
  state->last_free_entry = last_free_entry_;
  state->entries.assign(table_, table_ + table_capacity_);
  state->names = name_table_;
  for (const auto& entry : state->entries) {
    auto it = std::find_if(state->objects.begin(), state->objects.end(),
                           [&](const auto& item) { return item.first == entry.object; });
    if (entry.object && it == state->objects.end()) {
      XObject::RewindState object_state;
      entry.object->CaptureRewindState(&object_state);
      if (entry.object->type() == XObject::Type::File) {
        auto* file = static_cast<XFile*>(entry.object);
        state->file_positions.push_back({file, file->position()});
      } else if (entry.object->type() == XObject::Type::IOCompletion) {
        auto* port = static_cast<XIOCompletion*>(entry.object);
        XIOCompletion::QueueRewindState q;
        port->CaptureQueueRewindState(&q);
        state->completions.push_back({port, std::move(q)});
      } else if (entry.object->type() == XObject::Type::NotifyListener) {
        auto* listener = static_cast<XNotifyListener*>(entry.object);
        state->listeners.push_back({listener, listener->CaptureQueueRewindState()});
      }
      state->objects.push_back({retain_object(entry.object), std::move(object_state)});
    }
  }
  return state;
}

void ObjectTable::Rewind(const std::shared_ptr<RewindState>& state) {
  auto global_lock = global_critical_region_.Acquire();
  if (table_capacity_ < state->entries.size() && !Resize(state->entries.size())) {
    return;
  }
  // Every table slot owns one reference (AddHandle retains, RemoveHandle releases). An object the
  // game closed after the capture is alive only through our retain, so the slot we put back must
  // take its reference again; otherwise the next close after this load frees it and a later load
  // revives a dangling pointer. Objects created after the capture lose their slot and are leaked
  // on purpose (a thread may still hold them).
  uint32_t retained = 0;
  for (uint32_t slot = 0; slot < table_capacity_; ++slot) {
    if (slot < state->entries.size()) {
      table_[slot].handle_ref_count = state->entries[slot].handle_ref_count;
      XObject* want = state->entries[slot].object;
      if (want && table_[slot].object != want) {
        want->Retain();
        ++retained;
      }
      AtomicStore(table_[slot].object, want);
    } else {
      table_[slot].handle_ref_count = 0;
      AtomicStore(table_[slot].object, static_cast<XObject*>(nullptr));
    }
  }
  last_free_entry_ = state->last_free_entry;
  name_table_ = state->names;
  for (const auto& [object, object_state] : state->objects) {
    object->Rewind(object_state);
  }
  uint32_t moved = 0, locked = 0;
  for (const auto& [file, position] : state->file_positions) {
    // Parking waits for free file locks; if it gave up, skip rather than block forever.
    if (file->lock_held()) {
      ++locked;
      continue;
    }
    if (file->position() != position) {
      ++moved;
      file->set_position(position);
    }
  }
  std::fprintf(stderr, "[savepoint-s3] object table: %u slots re-took their reference\n", retained);
  std::fprintf(stderr, "[savepoint-s3] file positions: %zu files, %u rewound, %u skipped (lock held)\n",
               state->file_positions.size(), moved, locked);
  for (const auto& [port, q] : state->completions) {
    size_t before = port->RewindQueue(q);
    std::fprintf(stderr, "[savepoint-s3] io completion queue: %zu -> %zu\n", before, q.notifications.size());
  }
  for (const auto& [listener, q] : state->listeners) {
    size_t before = listener->RewindQueue(q);
    std::fprintf(stderr, "[savepoint-s3] notify listener queue: %zu -> %zu\n", before, q.size());
  }
}

void ObjectTable::Reset() {
  auto global_lock = global_critical_region_.Acquire();

  // Release all objects.
  for (uint32_t n = 0; n < table_capacity_; n++) {
    ObjectTableEntry& entry = table_[n];
    if (entry.object) {
      retired_objects_.push_back(entry.object);
      AtomicStore(entry.object, static_cast<XObject*>(nullptr));
    }
  }

  AtomicStore(table_capacity_, 0u);
  last_free_entry_ = 0;
  if (table_) {
    retired_tables_.push_back(table_);
  }
  AtomicStore(table_, static_cast<ObjectTableEntry*>(nullptr));
  ReleaseRetired();
}

X_STATUS ObjectTable::FindFreeSlot(uint32_t* out_slot) {
  // Find a free slot.
  uint32_t slot = last_free_entry_;
  uint32_t scan_count = 0;
  while (scan_count < table_capacity_) {
    ObjectTableEntry& entry = table_[slot];
    if (!entry.object) {
      *out_slot = slot;
      return X_STATUS_SUCCESS;
    }
    scan_count++;
    slot = (slot + 1) % table_capacity_;
    if (slot == 0) {
      // Never allow 0 handles.
      scan_count++;
      slot++;
    }
  }

  // Table out of slots, expand.
  uint32_t new_table_capacity = std::max(16 * 1024u, table_capacity_ * 2);
  if (!Resize(new_table_capacity)) {
    return X_STATUS_NO_MEMORY;
  }

  // Never allow 0 handles.
  slot = ++last_free_entry_;
  *out_slot = slot;

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Resize(uint32_t new_capacity) {
  uint32_t new_size = new_capacity * sizeof(ObjectTableEntry);
  uint32_t old_size = table_capacity_ * sizeof(ObjectTableEntry);
  // A copy, not realloc: lock-free lookups may still be reading the old table (see the top).
  auto new_table = reinterpret_cast<ObjectTableEntry*>(malloc(new_size));
  if (!new_table) {
    return false;
  }
  const uint32_t copied = std::min(old_size, new_size);
  if (table_ && copied) {
    std::memcpy(static_cast<void*>(new_table), table_, copied);
  }

  // Zero out new entries.
  if (new_size > copied) {
    std::memset(reinterpret_cast<uint8_t*>(new_table) + copied, 0, new_size - copied);
  }

  last_free_entry_ = table_capacity_;
  ObjectTableEntry* old_table = table_;
  // The table first, then its capacity: a lookup reads the capacity first, so it never indexes past the end.
  AtomicStore(table_, new_table);
  AtomicStore(table_capacity_, new_capacity);
  if (old_table) {
    retired_tables_.push_back(old_table);
    ReleaseRetired();
  }

  return true;
}

X_STATUS ObjectTable::AddHandle(XObject* object, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t handle = 0;
  {
    auto global_lock = global_critical_region_.Acquire();

    // Find a free slot.
    uint32_t slot = 0;
    result = FindFreeSlot(&slot);

    // Stash.
    if (XSUCCEEDED(result)) {
      ObjectTableEntry& entry = table_[slot];
      entry.handle_ref_count = 1;
      AtomicStore(entry.object, object);
      handle = XObject::kHandleBase + (slot << 2);
      object->handles().push_back(handle);

      // Retain so long as the object is in the table.
      object->Retain();

      REXSYS_NOISY_DEBUG("Added handle:{:08X} for {}", handle, typeid(*object).name());
    }
    ReleaseRetired();  // anything a busy RemoveHandle had to leave behind
  }

  if (XSUCCEEDED(result)) {
    if (out_handle) {
      *out_handle = handle;
    }
  }

  return result;
}

X_STATUS ObjectTable::DuplicateHandle(X_HANDLE handle, X_HANDLE* out_handle) {
  X_STATUS result = X_STATUS_SUCCESS;
  handle = TranslateHandle(handle);

  XObject* object = LookupObject(handle, false);
  if (object) {
    result = AddHandle(object, out_handle);
    object->Release();  // Release the ref that LookupObject took
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

X_STATUS ObjectTable::RetainHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  entry->handle_ref_count++;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleaseHandle(X_HANDLE handle) {
  auto global_lock = global_critical_region_.Acquire();

  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (--entry->handle_ref_count == 0) {
    // No more references. Remove it from the table.
    return RemoveHandle(handle);
  }

  // FIXME: Return a status code telling the caller it wasn't released
  // (but not a failure code)
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RemoveHandle(X_HANDLE handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  handle = TranslateHandle(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }

  // The lock before taking the entry pointer (a Resize in between used to leave it dangling).
  auto global_lock = global_critical_region_.Acquire();
  ObjectTableEntry* entry = LookupTable(handle);
  if (!entry) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (entry->object) {
    auto object = entry->object;
    AtomicStore(entry->object, static_cast<XObject*>(nullptr));
    assert_zero(entry->handle_ref_count);
    entry->handle_ref_count = 0;

    // Walk the object's handles and remove this one.
    auto handle_entry = std::find(object->handles().begin(), object->handles().end(), handle);
    if (handle_entry != object->handles().end()) {
      object->handles().erase(handle_entry);
    }

    REXSYS_NOISY_DEBUG("Removed handle:{:08X} for {}", handle, typeid(*object).name());

    // Remove object name from mapping to prevent naming collision.
    if (!object->name().empty()) {
      RemoveNameMapping(object->name());
    }
    // Release now that the object has been removed from the table, once no lookup can still be reading it.
    retired_objects_.push_back(object);
    ReleaseRetired();
  }

  return X_STATUS_SUCCESS;
}

std::vector<object_ref<XObject>> ObjectTable::GetAllObjects() {
  auto lock = global_critical_region_.Acquire();
  std::vector<object_ref<XObject>> results;

  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && std::find(results.begin(), results.end(), entry.object) == results.end()) {
      entry.object->Retain();
      results.push_back(object_ref<XObject>(entry.object));
    }
  }

  return results;
}

void ObjectTable::PurgeAllObjects() {
  auto lock = global_critical_region_.Acquire();
  for (uint32_t slot = 0; slot < table_capacity_; slot++) {
    auto& entry = table_[slot];
    if (entry.object && !entry.object->is_host_object()) {
      entry.handle_ref_count = 0;
      retired_objects_.push_back(entry.object);
      AtomicStore(entry.object, static_cast<XObject*>(nullptr));
    }
  }
  ReleaseRetired();
}

ObjectTable::ObjectTableEntry* ObjectTable::LookupTable(X_HANDLE handle) {
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Lower 2 bits are ignored.
  uint32_t slot = GetHandleSlot(handle);
  if (slot < table_capacity_) {
    return &table_[slot];
  }

  return nullptr;
}

// Generic lookup
template <>
object_ref<XObject> ObjectTable::LookupObject<XObject>(X_HANDLE handle) {
  auto object = ObjectTable::LookupObject(handle, false);
  auto result = object_ref<XObject>(reinterpret_cast<XObject*>(object));
  return result;
}

XObject* ObjectTable::LookupObject(X_HANDLE handle, bool already_locked) {
  (void)already_locked;  // lookups take no lock (see the top of this file)
  handle = TranslateHandle(handle);
  if (!handle) {
    return nullptr;
  }

  XObject* object = nullptr;
  lookups_in_flight_.fetch_add(1, std::memory_order_seq_cst);

  // Lower 2 bits are ignored.
  uint32_t slot = GetHandleSlot(handle);

  // Verify slot. The capacity before the table: Resize publishes them the other way round.
  const uint32_t capacity = AtomicLoad(table_capacity_);
  if (slot < capacity) {
    ObjectTableEntry* table = AtomicLoad(table_);
    object = AtomicLoad(table[slot].object);
  }

  // Retain the object pointer.
  if (object) {
    object->Retain();
  }

  lookups_in_flight_.fetch_sub(1, std::memory_order_seq_cst);
  return object;
}

void ObjectTable::GetObjectsByType(XObject::Type type, std::vector<object_ref<XObject>>* results) {
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t slot = 0; slot < table_capacity_; ++slot) {
    auto& entry = table_[slot];
    if (entry.object) {
      if (entry.object->type() == type) {
        entry.object->Retain();
        results->push_back(object_ref<XObject>(entry.object));
      }
    }
  }
}

X_HANDLE ObjectTable::TranslateHandle(X_HANDLE handle) {
  if (handle == 0xFFFFFFFF) {
    // CurrentProcess
    // assert_always();
    return 0;
  } else if (handle == 0xFFFFFFFE) {
    // CurrentThread
    return XThread::GetCurrentThreadHandle();
  } else {
    return handle;
  }
}

X_STATUS ObjectTable::AddNameMapping(const std::string_view name, X_HANDLE handle) {
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  if (name_table_.count(string::string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  name_table_.insert({string::string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveNameMapping(const std::string_view name) {
  // Names are case-insensitive.
  std::lock_guard<std::mutex> name_lock(name_mutex_);
  auto it = name_table_.find(string::string_key_case(name));
  if (it != name_table_.end()) {
    name_table_.erase(it);
  }
}

X_STATUS ObjectTable::GetObjectByName(const std::string_view name, X_HANDLE* out_handle) {
  // Names are case-insensitive.
  // Look up handle under name lock only -- do NOT hold name_mutex_ while
  // acquiring global lock (RemoveHandle takes global -> name ordering).
  X_HANDLE handle;
  {
    std::lock_guard<std::mutex> name_lock(name_mutex_);
    auto it = name_table_.find(string::string_key_case(name));
    if (it == name_table_.end()) {
      *out_handle = X_INVALID_HANDLE_VALUE;
      return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    handle = it->second;
  }
  *out_handle = handle;

  // Retain under global lock via normal LookupObject path.
  // The handle may have been removed between releasing name_mutex_ and
  // acquiring global lock -- LookupObject returns nullptr in that case.
  auto obj = LookupObject(handle, false);
  if (obj) {
    obj->RetainHandle();
    obj->Release();
  }

  return X_STATUS_SUCCESS;
}

bool ObjectTable::Save(stream::ByteStream* stream) {
  stream->Write<uint32_t>(table_capacity_);
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    stream->Write<int32_t>(entry.handle_ref_count);
  }

  return true;
}

bool ObjectTable::Restore(stream::ByteStream* stream) {
  Resize(stream->Read<uint32_t>());
  for (uint32_t i = 0; i < table_capacity_; i++) {
    auto& entry = table_[i];
    // entry.object = nullptr;
    entry.handle_ref_count = stream->Read<int32_t>();
  }

  return true;
}

X_STATUS ObjectTable::RestoreHandle(X_HANDLE handle, XObject* object) {
  uint32_t slot = GetHandleSlot(handle);
  assert_true(table_capacity_ > slot);

  if (table_capacity_ > slot) {
    auto& entry = table_[slot];
    object->Retain();
    AtomicStore(entry.object, object);
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::system::util
