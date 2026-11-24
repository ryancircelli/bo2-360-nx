#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <queue>

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

namespace rex::system {

class XIOCompletion : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::IOCompletion;

  explicit XIOCompletion(KernelState* kernel_state);
  ~XIOCompletion() override;

  struct IONotification {
    uint32_t key_context;
    uint32_t apc_context;
    uint32_t status;
    uint32_t num_bytes;
  };

  void QueueNotification(IONotification& notification);

  // Returns true if the wait ended because a notification was received.
  bool WaitForNotification(uint64_t wait_ticks, IONotification* notify);

  // Savepoint rewind: the queue and its semaphore count are host-side, outside guest memory.
  struct QueueRewindState {
    std::queue<IONotification> notifications;
    rex::thread::WaitHandleRewindState semaphore;
    bool has_semaphore = false;
  };
  void CaptureQueueRewindState(QueueRewindState* state) {
    std::lock_guard<std::mutex> lock(notification_lock_);
    state->notifications = notifications_;
    state->has_semaphore =
        rex::thread::CaptureWaitHandleRewindState(notification_semaphore_.get(), &state->semaphore);
  }
  size_t RewindQueue(const QueueRewindState& state) {
    std::lock_guard<std::mutex> lock(notification_lock_);
    size_t before = notifications_.size();
    notifications_ = state.notifications;
    if (state.has_semaphore) rex::thread::RewindWaitHandle(notification_semaphore_.get(), state.semaphore);
    return before;
  }

 private:
  static const uint32_t kMaxNotifications = 1024;

  std::mutex notification_lock_;
  std::queue<IONotification> notifications_;
  std::unique_ptr<rex::thread::Semaphore> notification_semaphore_ = nullptr;
};

}  // namespace rex::system
