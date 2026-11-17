// SPDX-License-Identifier: BSD-3-Clause
// v0.10 rex/system/xthread.h subset on v0.11: the calling thread's id and host thread.
#pragma once
#include <cstdint>
#include <rex/thread.h>
namespace rex::system {
class XThread {
 public:
  static uint32_t GetCurrentThreadId() { return uint32_t(rex::thread::Thread::GetCurrentThread()->system_id()); }
  static XThread* GetCurrentThread() {
    static thread_local XThread self;
    return &self;
  }
  rex::thread::Thread* thread() { return rex::thread::Thread::GetCurrentThread(); }
};
}  // namespace rex::system
