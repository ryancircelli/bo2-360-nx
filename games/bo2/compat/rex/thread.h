// SPDX-License-Identifier: BSD-3-Clause
// v0.10 rex/thread.h subset on v0.11: thread names and the audio scheduling class.
#pragma once
#include <pthread.h>
#include <cstdint>
#include <string>
#include <rex/thread_audio.h>
namespace rex::thread {
inline void set_current_thread_name(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}
class Thread {
 public:
  static Thread* GetCurrentThread() {
    static thread_local Thread self;
    return &self;
  }
  std::string name() const {
    char buf[64] = {};
    pthread_getname_np(pthread_self(), buf, sizeof(buf));
    return buf;
  }
  uint64_t system_id() const {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
  }
};
inline std::string SetThreadAudioPriority() { return SetThreadAudioPriority("realtime", nullptr); }
}  // namespace rex::thread
