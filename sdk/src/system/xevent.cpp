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

#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>
#include <rex/system/thread_state.h>

#include <chrono>
#include <dlfcn.h>
#include <execinfo.h>
#include <cstdio>
#include <map>
#include <tuple>
#include <mutex>
#include <string>
#include <cstdlib>
#include <cstring>

namespace rex::system {

XEvent::XEvent(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  this->CreateNative<X_KEVENT>();

  if (manual_reset) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case 0x00:  // EventNotificationObject (manual reset)
      manual_reset_ = true;
      break;
    case 0x01:  // EventSynchronizationObject (auto reset)
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  if (out_type) {
    *out_type = manual_reset_ ? 0x00 : 0x01;
  }
  if (out_state) {
    // Query the live host event, not the stale guest header
    auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
    if (result == rex::thread::WaitResult::kSuccess) {
      *out_state = 1;
      // Re-signal since we consumed the signal by waiting
      event_->Set();
    } else {
      *out_state = 0;
    }
  }
}

namespace {
// REX_TRACE_EVENT=F8000058: log who sets or pulses that event (thread handle, guest lr). Diagnosis only.
void TraceEvent(const XEvent* e, const char* what) {
  static const char* list = std::getenv("REX_TRACE_EVENT");  // comma-separated hex handles, or * for counts
  if (!list) return;
  if (list[0] == '*') {  // count sets per (event, guest caller); dump every 10 s
    static std::mutex m;
    static std::map<std::string, uint32_t> counts;
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::string> callers;  // backtrace once per key
    static auto last = std::chrono::steady_clock::now();
    auto* t = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
    const auto key = std::make_tuple(e->handle(), t ? t->handle() : 0u,
                                     t ? uint32_t(t->thread_state()->context()->lr) : 0u);
    std::string caller;
    {
      std::lock_guard<std::mutex> l(m);
      auto it = callers.find(key);
      if (it != callers.end()) caller = it->second;
    }
    if (caller.empty()) {
      // The second recompiled game function up the host stack (the first is the game's NtSetEvent wrapper).
      void* frames[24];
      int n = ::backtrace(frames, 24), games = 0;
      caller = "?";
      for (int i = 0; i < n; ++i) {
        Dl_info di{};
        if (!dladdr(frames[i], &di) || !di.dli_sname || std::strncmp(di.dli_sname, "__imp__sub_", 11) != 0) continue;
        if (++games == 2) {
          caller = di.dli_sname + 7;
          break;
        }
      }
      std::lock_guard<std::mutex> l(m);
      callers[key] = caller;
    }
    std::lock_guard<std::mutex> l(m);
    ++counts[fmt::format("{:08X}/{:08X}/{}", e->handle(), t ? t->handle() : 0u, caller)];
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    std::string line;
    for (auto& [k, n] : counts) line += fmt::format(" {}x{}", k, n);
    REXSYS_INFO("[trace-event] counts{}", line);
    counts.clear();
    return;
  }
  char hex[16];
  std::snprintf(hex, sizeof hex, "%08X", e->handle());
  if (!std::strstr(list, hex)) return;
  const uint32_t traced = e->handle();
  auto* t = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  REXSYS_INFO("[trace-event] {} {:08X} by thread {:08X} lr {:08X}", what, traced, t ? t->handle() : 0u,
              t ? uint32_t(t->thread_state()->context()->lr) : 0u);
}
}  // namespace

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  TraceEvent(this, "set");
  event_->Set();
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  TraceEvent(this, "pulse");
  event_->Pulse();
  return 1;
}

int32_t XEvent::Reset() {
  event_->Reset();
  return 1;
}

void XEvent::Clear() {
  event_->Reset();
}

bool XEvent::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  bool signaled = true;
  auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
  if (result == rex::thread::WaitResult::kSuccess) {
    signaled = true;
  } else if (result == rex::thread::WaitResult::kTimeout) {
    signaled = false;
  } else {
    assert_always();
  }

  if (signaled) {
    // Reset the event in-case it's an auto-reset.
    event_->Set();
  }

  stream->Write<bool>(signaled);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();

  if (evt->manual_reset_) {
    evt->event_ = rex::thread::Event::CreateManualResetEvent(false);
  } else {
    evt->event_ = rex::thread::Event::CreateAutoResetEvent(false);
  }
  assert_not_null(evt->event_);

  if (signaled) {
    evt->event_->Set();
  }

  return object_ref<XEvent>(evt);
}

}  // namespace rex::system
