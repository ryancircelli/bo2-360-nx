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

#include <pthread.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/stream.h>
#include <rex/thread_spin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsocket.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

namespace rex::system {

namespace {

// The handle stash in a guest dispatcher header, read by GetNativeObject without the global lock.
// The handle (blink) is written first and the signature (flink) last with release order, so a reader that sees
// the signature also sees the handle. XObject::StashHandle (xobject.h) writes them the other way round.
void PublishStash(X_DISPATCH_HEADER* header, uint32_t handle) {
  std::atomic_ref<uint32_t>(header->wait_list_blink.value)
      .store(rex::byte_swap(handle), std::memory_order_relaxed);
  std::atomic_ref<uint32_t>(header->wait_list_flink.value)
      .store(rex::byte_swap(static_cast<uint32_t>(kXObjSignature)), std::memory_order_release);
}

// Bodies the SDK allocated with CreateNative, by guest address, to their owner. Some of these bodies
// (enumerators) have no dispatch header: their guest fields overwrite the stash words, so GetNativeObject must
// find their owner here instead of mistaking the body for an event it has never seen.
std::mutex g_created_bodies_mutex;
std::unordered_map<uint32_t, XObject*> g_created_bodies;

// True when guest addresses a and b are the same object body, also through the physical-memory views
// (0xA0000000/0xC0000000/0xE0000000 map the same pages).
bool SameBody(KernelState* kernel_state, uint32_t a, uint32_t b) {
  if (a == b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  const uint32_t pa = kernel_state->memory()->GetPhysicalAddress(a);
  return pa != UINT32_MAX && pa == kernel_state->memory()->GetPhysicalAddress(b);
}

bool ReadStash(X_DISPATCH_HEADER* header, uint32_t* handle) {
  const uint32_t flink = rex::byte_swap(
      std::atomic_ref<uint32_t>(header->wait_list_flink.value).load(std::memory_order_acquire));
  if (flink != static_cast<uint32_t>(kXObjSignature)) {
    return false;
  }
  *handle = rex::byte_swap(
      std::atomic_ref<uint32_t>(header->wait_list_blink.value).load(std::memory_order_relaxed));
  return true;
}

}  // namespace

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    {
      std::lock_guard<std::mutex> lock(g_created_bodies_mutex);
      g_created_bodies.erase(guest_object_ptr_);
    }
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

void XObject::Retain() {
  ++pointer_ref_count_;
}

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);
  stream->Write<uint32_t>(wrapped_native_ptr_);  // else GetNativeObject no longer trusts the stash after a restore

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();
  wrapped_native_ptr_ = stream->Read<uint32_t>();
  if (allocated_guest_object_ && guest_object_ptr_) {
    std::lock_guard<std::mutex> lock(g_created_bodies_mutex);
    g_created_bodies[guest_object_ptr_] = this;
  }

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      return XSocket::Restore(kernel_state, stream);
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      return XTimer::Restore(kernel_state, stream);
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601: wait until then (0 once it has passed).
    const uint64_t now = chrono::Clock::QueryGuestSystemTime();
    return uint64_t(timeout_ticks) > now ? uint32_t((uint64_t(timeout_ticks) - now) / 10000) : 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

REXCVAR_DEFINE_INT32(subms_wait_mode, 0, "Kernel",
                     "Bench knob: a Wait with a guest timeout under 1 ms (truncated to 0 ms, so a bare poll). 0 = old "
                     "behaviour, 1 = count it as a guest poll (yield + spin backoff, like the other waits do), 2 = wait "
                     "1 ms instead");

// Wait-loop study item 3: a nonzero guest timeout under 1 ms is truncated to 0 ms, so the wait turns into a poll.
// Counts those per thread and logs them every 10 s ("[subms]"). Also called from XThread::Delay.
void NoteSubMsTimeout(const char* where) {
  struct Count {
    uint64_t n = 0;
    std::chrono::steady_clock::time_point window{};
  };
  static thread_local Count c;
  const auto now = std::chrono::steady_clock::now();
  if (c.window == std::chrono::steady_clock::time_point{}) c.window = now;
  ++c.n;
  if (now - c.window >= std::chrono::seconds(10)) {
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    REXSYS_INFO("[subms] {}, last 10 s: {} {} calls with a guest timeout under 1 ms truncated to 0", name, c.n, where);
    c.n = 0;
    c.window = now;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();
  const bool subms = opt_timeout && *opt_timeout != 0 && timeout_ms.count() == 0;
  if (subms) {
    NoteSubMsTimeout("Wait");
    if (REXCVAR_GET(subms_wait_mode) == 2) timeout_ms = std::chrono::milliseconds(1);
  }

  XThread::CheckTitleTermination();
  auto result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout_ms);
  XThread::CheckTitleTermination();
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      // Only after a zero-timeout poll. After a real timed wait the thread has already given up
      // the core for the whole timeout.
      if (!opt_timeout || *opt_timeout == 0 || (subms && REXCVAR_GET(subms_wait_mode) == 1)) {
        rex::thread::GuestPoll();
      }
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      // Only after a zero-timeout poll, as Wait: after a real timed wait the thread already gave up the core.
      if (timeout_ms.count() == 0) rex::thread::GuestPoll();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  // No heap allocation for the usual counts (every malloc takes newlib's global heap lock on the
  // Switch). The Xbox allows at most 64 objects per wait.
  constexpr uint32_t kInlineHandles = 64;
  rex::thread::WaitHandle* inline_handles[kInlineHandles];
  std::vector<rex::thread::WaitHandle*> heap_handles;
  rex::thread::WaitHandle** wait_handles = inline_handles;
  if (count > kInlineHandles) {
    heap_handles.resize(count);
    wait_handles = heap_handles.data();
  }
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  if (wait_type) {
    auto result =
        rex::thread::WaitAny(wait_handles, count, alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        if (timeout_ms.count() == 0) rex::thread::GuestPoll();  // as Wait: no yield after a real timed wait
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result = rex::thread::WaitAll(wait_handles, count, alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        if (timeout_ms.count() == 0) rex::thread::GuestPoll();  // as Wait: no yield after a real timed wait
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);
  {
    std::lock_guard<std::mutex> lock(g_created_bodies_mutex);
    g_created_bodies[guest_object_ptr_] = this;
  }

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  PublishStash(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::FindCreatedBody(KernelState* kernel_state, uint32_t body) {
  // ~XObject removes its body under this mutex before freeing anything, so an entry found here is still a live
  // allocation; take a reference only if the object is not already on its way to deletion.
  std::lock_guard<std::mutex> lock(g_created_bodies_mutex);
  auto it = g_created_bodies.find(body);
  if (it == g_created_bodies.end()) {
    return nullptr;
  }
  XObject* object = it->second;
  int32_t count = object->pointer_ref_count_.load(std::memory_order_acquire);
  while (count > 0) {
    if (object->pointer_ref_count_.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) {
      return object_ref<XObject>(object);
    }
  }
  return nullptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  uint32_t handle = 0;

  // Already set up (every call after the first) needs no lock: the stash and the handle lookup are
  // both lock-free. Only the first use, which creates the object, takes the global critical region.
  // A stash only counts if the handle still names the object made for this body. Without the check a stale stash
  // (the handle was freed and the slot reused) hands back an unrelated object, and ObDereferenceObject then
  // releases that object's handle: an event the game is still using dies with no NtClose.
  const uint32_t body = kernel_state->memory()->HostToGuestVirtual(native_ptr);
  if (ReadStash(header, &handle)) {
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);
    if (object && SameBody(kernel_state, object->native_body(), body)) {
      return object;
    }
  }

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // A body the SDK allocated belongs to its object, whatever its first bytes look like.
  if (auto object = FindCreatedBody(kernel_state, body)) {
    return object;
  }

  if (as_type == -1) {
    as_type = header->type;
  }

  object_ref<XObject> stashed;
  if (ReadStash(header, &handle)) {
    stashed = kernel_state->object_table()->LookupObject<XObject>(handle);
  }
  if (stashed && SameBody(kernel_state, stashed->native_body(), body)) {
    // Already initialized.
    return stashed;
  } else {
    // First use (or a stale stash), create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    if (std::getenv("REX_TRACE_EVENT")) {  // diagnosis: a stash that did not match makes a second object
      REXSYS_INFO("[trace-event] new native object type {} body {:08X} stash {:08X} stashed body {:08X}", as_type,
                  body, handle, stashed ? stashed->native_body() : 0u);
    }
    // Stash pointer in struct. Bodies without a dispatch header are SDK-allocated and were returned above.
    object->wrapped_native_ptr_ = body;
    PublishStash(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
