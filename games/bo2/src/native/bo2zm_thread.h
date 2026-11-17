// SPDX-License-Identifier: BSD-3-Clause
// Names the calling thread (v0.10 had rex::thread::set_current_thread_name).
#pragma once

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace bo2zm {

inline void SetThreadName(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

}  // namespace bo2zm
