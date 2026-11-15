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

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/stream.h>
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
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <cstdio>

REXCVAR_DEFINE_STRING(wait_watchdog_regpeek, "", "Kernel",
                      "Diagnostics: 'lr:off:len[,...]' (hex): for each waiter whose guest lr matches, dump "
                      "len bytes at its r31+off in every snapshot");
REXCVAR_DEFINE_STRING(wait_watchdog_peek, "", "Kernel",
                      "Diagnostics: guest memory to hex-dump in each wait watchdog snapshot, comma separated "
                      "'addr:len' or '[ptr]+off:len' (one dereference), hex addresses");
REXCVAR_DEFINE_INT32(wait_watchdog_s, 0, "Kernel",
                     "Diagnostics: log guest threads blocked in a kernel wait for longer than this "
                     "many seconds, with each object and who last signalled it (0 = off)");

namespace rex::system {

std::string (*g_diag_extra_dump)() = nullptr;


namespace {
// Wait watchdog (PAS-18). Off by default; when wait_watchdog_s > 0 every guest kernel wait is
// recorded with the objects it waits on, every signal records who sent it, and a host thread logs
// a snapshot of all waiting guest threads once any of them has waited longer than the limit.
int64_t DiagNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t DiagGuestLr() {
  if (!XThread::IsInThread()) return 0;
  auto* ts = XThread::GetCurrentThread()->thread_state();
  return ts && ts->context() ? static_cast<uint32_t>(ts->context()->lr) : 0;
}

// Guest memory read for diagnostics: 0 if the page is not accessible.
bool DiagRead32(uint32_t addr, uint32_t* out) {
  auto* mem = kernel_state()->memory();
  auto* heap = mem->LookupHeap(addr);
  if (!heap || (addr & 3) ||
      heap->QueryRangeAccess(addr, addr + 3) == rex::memory::PageAccess::kNoAccess) {
    return false;
  }
  *out = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(addr));
  return true;
}

// Walk the guest back chain (r1 -> caller sp; saved lr at caller_sp - 8, the 360 ABI) of the
// current guest thread. Only reads inside the thread's own stack allocation.
size_t DiagGuestStack(uint32_t* out, size_t max) {
  if (!XThread::IsInThread()) return 0;
  auto* t = XThread::GetCurrentThread();
  auto* ts = t->thread_state();
  if (!ts || !ts->context()) return 0;
  uint32_t lo = t->diag_stack_low(), hi = t->diag_stack_high();
  uint32_t sp = ts->context()->r1.u32;
  size_t n = 0;
  while (n < max && sp >= lo && sp + 4 <= hi) {
    uint32_t back = 0, lr = 0;
    // The thread's own stack allocation is committed: plain loads, no heap lock (this also runs on
    // every signal while the watchdog is on).
    auto* mem = kernel_state()->memory();
    back = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(sp));
    if (back <= sp || back < lo + 8 || back >= hi) break;  // hi is the first guard page
    lr = memory::load_and_swap<uint32_t>(mem->TranslateVirtual(back - 8));
    out[n++] = lr;
    sp = back;
  }
  return n;
}

std::string DiagStackString(const uint32_t* s, size_t n) {
  std::string r;
  for (size_t i = 0; i < n; ++i) r += fmt::format("{}{:08X}", i ? "<" : "", s[i]);
  return r;
}

std::string DiagDump(uint32_t addr, uint32_t len) {
  std::string out;
  for (uint32_t i = 0; i < std::min<uint32_t>(len, 0x400); i += 4) {
    uint32_t v = 0;
    if (i % 32 == 0) out += fmt::format("\n    +{:03X}", i);
    out += DiagRead32(addr + i, &v) ? fmt::format(" {:08X}", v) : std::string(" ????????");
  }
  return out;
}

std::string DiagRegPeek(uint32_t lr, uint32_t r31) {
  std::string spec = REXCVAR_GET(wait_watchdog_regpeek), out;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    pos = end + 1;
    uint32_t want = 0, off = 0, len = 0;
    if (std::sscanf(item.c_str(), "%x:%x:%x", &want, &off, &len) != 3 || want != lr) continue;
    out += fmt::format("\n  r31={:08X}+{:X}:", r31, off) + DiagDump(r31 + off, len);
  }
  return out;
}

std::string DiagPeek() {
  std::string spec = REXCVAR_GET(wait_watchdog_peek);
  std::string out;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    uint32_t len = std::strtoul(item.c_str() + colon + 1, nullptr, 16);
    std::string a = item.substr(0, colon);
    uint32_t addr = 0;
    if (!a.empty() && a[0] == '[') {
      size_t close = a.find(']');
      if (close == std::string::npos) continue;
      uint32_t ptr = std::strtoul(a.c_str() + 1, nullptr, 16), v = 0;
      int32_t off = 0;
      if (close + 1 < a.size()) off = int32_t(std::strtol(a.c_str() + close + 1, nullptr, 16));
      if (!DiagRead32(ptr, &v)) { out += fmt::format("\n  {} -> unreadable", item); continue; }
      addr = v + off;
    } else {
      addr = std::strtoul(a.c_str(), nullptr, 16);
    }
    out += fmt::format("\n  {} @{:08X}:", item, addr);
    for (uint32_t i = 0; i < std::min<uint32_t>(len, 0x400); i += 4) {
      uint32_t v = 0;
      if (i % 32 == 0) out += fmt::format("\n    +{:03X}", i);
      out += DiagRead32(addr + i, &v) ? fmt::format(" {:08X}", v) : std::string(" ????????");
    }
  }
  return out;
}

const char* DiagTypeName(XObject::Type t) {
  static const char* kNames[] = {"Undefined", "Enumerator", "Event",  "File",
                                 "IOCompletion", "Module", "Mutant", "NotifyListener",
                                 "Semaphore", "Session", "Socket", "SymbolicLink",
                                 "Thread", "Timer"};
  auto i = static_cast<uint32_t>(t);
  return i < std::size(kNames) ? kNames[i] : "?";
}

struct DiagWait {
  std::vector<object_ref<XObject>> objects;
  int64_t start_ms = 0;
  uint32_t lr = 0;
  uint32_t stack[12] = {};
  size_t stack_n = 0;
  uint32_t thread_id = 0;
  std::string thread_name;
  bool alertable = false;
  bool wait_all = false;
  int64_t timeout_ticks = 0;
  bool has_timeout = false;
  bool reported = false;
  uint32_t r31 = 0;
};

std::mutex g_diag_mutex;
std::unordered_map<XThread*, DiagWait> g_diag_waits;
std::once_flag g_diag_once;

void DiagWatchdogMain() {
  int64_t last_snapshot_ms = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    int32_t limit_s = REXCVAR_GET(wait_watchdog_s);
    if (limit_s <= 0) continue;
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    int64_t now = DiagNowMs();
    bool any_new = false;
    for (auto& [t, w] : g_diag_waits) {
      if (!w.reported && now - w.start_ms >= int64_t(limit_s) * 1000) any_new = true;
    }
    bool any_over = false;
    for (auto& [t, w] : g_diag_waits) {
      if (now - w.start_ms >= int64_t(limit_s) * 1000) any_over = true;
    }
    // New stuck waits: at most every 30 s. Still stuck: a repeat snapshot every 120 s.
    if (!(any_new && now - last_snapshot_ms >= 30000) && !(any_over && now - last_snapshot_ms >= 120000)) {
      continue;
    }
    last_snapshot_ms = now;
    REXSYS_WARN("[waitdog] snapshot: {} guest threads in kernel waits{}{}", g_diag_waits.size(),
                DiagPeek(), g_diag_extra_dump ? g_diag_extra_dump() : std::string());
    for (auto& [t, w] : g_diag_waits) {
      bool over = now - w.start_ms >= int64_t(limit_s) * 1000;
      if (over) w.reported = true;
      std::string objs;
      for (auto& o : w.objects) {
        auto& sig = o->diag_signal();
        uint64_t n = sig.count.load(std::memory_order_relaxed);
        int64_t last = sig.last_ms.load(std::memory_order_relaxed);
        std::string extra;
        if (o->type() == XObject::Type::Thread) {
          extra = fmt::format(" thread='{}'", static_cast<XThread*>(o.get())->thread_name());
        }
        objs += fmt::format(
            " [{} handle={:08X} guest={:08X} name='{}'{} signals={} last={} by tid={:X} lr={:08X}<{}]",
            DiagTypeName(o->type()), o->handles().empty() ? 0u : o->handle(), o->guest_object(),
            o->name(), extra, n, n ? fmt::format("{}ms ago", now - last) : std::string("never"),
            sig.thread_id.load(std::memory_order_relaxed), sig.lr.load(std::memory_order_relaxed),
            [&] {
              uint32_t s[6];
              for (int i = 0; i < 6; ++i) s[i] = sig.stack[i].load(std::memory_order_relaxed);
              size_t k = 0;
              while (k < 6 && s[k]) ++k;
              return DiagStackString(s, k);
            }());
      }
      REXSYS_WARN("[waitdog] {}{} tid={:X} waiting {}ms at lr={:08X} r31={:08X} stack={} {}{} timeout={}:{}{}{}",
                  over ? "STUCK " : "", w.thread_name, w.thread_id, now - w.start_ms, w.lr, w.r31,
                  DiagStackString(w.stack, w.stack_n), w.wait_all ? "all" : "any",
                  w.alertable ? " alertable" : "", w.has_timeout ? "ticks" : "infinite",
                  w.timeout_ticks, objs, DiagRegPeek(w.lr, w.r31));
    }
  }
}

class DiagWaitScope {
 public:
  DiagWaitScope(XObject* const* objects, uint32_t count, bool wait_all, uint32_t alertable,
                uint64_t* opt_timeout) {
    if (REXCVAR_GET(wait_watchdog_s) <= 0 || !XThread::IsInThread()) return;
    thread_ = XThread::GetCurrentThread();
    std::call_once(g_diag_once, [] { std::thread(DiagWatchdogMain).detach(); });
    DiagWait w;
    for (uint32_t i = 0; i < count; ++i) w.objects.push_back(retain_object(objects[i]));
    w.start_ms = DiagNowMs();
    w.lr = DiagGuestLr();
    {
      auto* ts = thread_->thread_state();
      w.r31 = ts && ts->context() ? ts->context()->r31.u32 : 0;
    }
    w.stack_n = DiagGuestStack(w.stack, std::size(w.stack));
    w.thread_id = thread_->thread_id();
    w.thread_name = thread_->thread_name();
    w.alertable = alertable != 0;
    w.wait_all = wait_all;
    w.has_timeout = opt_timeout != nullptr;
    w.timeout_ticks = opt_timeout ? int64_t(*opt_timeout) : 0;
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    g_diag_waits[thread_] = std::move(w);
  }
  ~DiagWaitScope() {
    if (!thread_) return;
    DiagWait old;
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      auto it = g_diag_waits.find(thread_);
      if (it == g_diag_waits.end()) return;
      old = std::move(it->second);
      g_diag_waits.erase(it);
    }
    if (old.reported) {
      REXSYS_WARN("[waitdog] {} woke after {}ms (lr={:08X})", old.thread_name,
                  DiagNowMs() - old.start_ms, old.lr);
    }
    // old.objects released here, on the waiting guest thread.
  }

 private:
  XThread* thread_ = nullptr;
};
}  // namespace

void XObject::DiagNoteSignal() {
  if (REXCVAR_GET(wait_watchdog_s) <= 0) return;
  diag_signal_.count.fetch_add(1, std::memory_order_relaxed);
  diag_signal_.last_ms.store(DiagNowMs(), std::memory_order_relaxed);
  diag_signal_.thread_id.store(XThread::IsInThread() ? XThread::GetCurrentThread()->thread_id() : 0,
                               std::memory_order_relaxed);
  diag_signal_.lr.store(DiagGuestLr(), std::memory_order_relaxed);
  uint32_t s[6] = {};
  DiagGuestStack(s, 6);
  for (int i = 0; i < 6; ++i) diag_signal_.stack[i].store(s[i], std::memory_order_relaxed);
}

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

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

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
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
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
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
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

  XObject* diag_self = this;
  DiagWaitScope diag_scope(&diag_self, 1, false, alertable, opt_timeout);
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
      if (!opt_timeout || *opt_timeout == 0) {
        rex::thread::MaybeYield();
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

  signal_object->DiagNoteSignal();
  DiagWaitScope diag_scope(&wait_object, 1, false, alertable, opt_timeout);
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
      rex::thread::MaybeYield();
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

  DiagWaitScope diag_scope(objects, count, wait_type == 0, alertable, opt_timeout);
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
        rex::thread::MaybeYield();
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
        rex::thread::MaybeYield();
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
  if (ReadStash(header, &handle)) {
    if (auto object = kernel_state->object_table()->LookupObject<XObject>(handle)) {
      return object;
    }
  }

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  if (as_type == -1) {
    as_type = header->type;
  }

  if (ReadStash(header, &handle)) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
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

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    PublishStash(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
