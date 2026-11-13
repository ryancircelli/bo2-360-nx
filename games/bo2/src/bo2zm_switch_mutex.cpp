// Switch only: contended host mutexes spin briefly before sleeping in the kernel.
//
// Why: an early profile on the Switch, with Town running at 0-4 fps, had the game threads spending ~75 % of their
// samples in svcArbitrateLock, called from pthread_mutex_lock. The recompiled code turns each
// "mtmsrd r13" (interrupts off, around the game's lwarx/stwcx. atomics) into REX_ENTER_GLOBAL_LOCK(), a single
// std::recursive_mutex shared by every thread and by the runtime (rex::thread::global_critical_region), and
// mfmsr into a lock/unlock of it as well. Those sections are a few instructions long, but newlib/libnx's mutex
// sleeps in the kernel at the first contention (svcArbitrateLock) and wakes through svcArbitrateUnlock; with 7+
// busy guest threads on 3 cores the threads mostly waited for each other. Windows' locks spin a little first,
// which is why the PC build does not show it.
//
// What: the linker routes every pthread_mutex_lock to this wrapper (-Wl,--wrap=pthread_mutex_lock, CMakeLists).
// It retries pthread_mutex_trylock a bounded number of times (a few microseconds) and only then blocks as before.
// Same mutexes, same ownership and recursion rules (trylock on a recursive mutex the thread owns succeeds); only
// the waiting strategy changes.

#if defined(__SWITCH__)

#include <pthread.h>
#include <sys/lock.h>

#include <atomic>

#include "bo2zm_benchmark.h"
#include "bo2zm_switch_mutex.h"

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  // ~64 attempts with a short pause each: a few microseconds at 1 GHz, longer than the critical sections of the
  // guest atomics, much shorter than a kernel sleep and wake-up.
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (pthread_mutex_trylock(mutex) == 0) {
      return 0;
    }
    for (int pause = 0; pause < 8; ++pause) {
      __asm__ __volatile__("yield");
    }
  }
  return __real_pthread_mutex_lock(mutex);
}

// The C heap's lock: newlib's malloc, free, realloc and memalign take it through __malloc_lock / __malloc_unlock,
// and its own lock (a static recursive one in newlib's mlock.c) sleeps in the kernel at the first contention, like
// the mutexes above. On a map's first visit about 20 % of the pipeline compile thread's samples waited on it
// (svcArbitrateLock under _malloc_r / _free_r, from NIR and NAK). The linker routes both functions here
// (-Wl,--wrap=__malloc_lock,--wrap=__malloc_unlock, CMakeLists): one recursive lock of our own replaces newlib's
// (nothing else uses that one), and SetMallocSpin(true) gives it the same bounded spin before the kernel wait;
// SetMallocSpinFactor makes the spin a benchmark A/B factor. The spin is off by default because it measured no gain,
// neither in a match nor on a first visit; without it the lock behaves exactly like newlib's own.
namespace {
_LOCK_RECURSIVE_T g_malloc_lock = __LOCK_INITIALIZER_RECURSIVE;
std::atomic<bool> g_malloc_spin{false};
std::atomic<int> g_malloc_spin_factor{0};

bool MallocSpin() {
  if (const int factor = g_malloc_spin_factor.load(std::memory_order_relaxed)) {
    const int arm = bo2zm::benchmark::AbArmCached();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return g_malloc_spin.load(std::memory_order_relaxed);
}
}  // namespace

extern "C" void __wrap___malloc_lock(struct _reent*) {
  if (MallocSpin()) {
    for (int attempt = 0; attempt < 64; ++attempt) {
      if (__libc_lock_try_acquire_recursive(&g_malloc_lock) == 0) {
        return;
      }
      for (int pause = 0; pause < 8; ++pause) {
        __asm__ __volatile__("yield");
      }
    }
  }
  __libc_lock_acquire_recursive(&g_malloc_lock);
}

extern "C" void __wrap___malloc_unlock(struct _reent*) {
  __libc_lock_release_recursive(&g_malloc_lock);
}

namespace bo2zm {
void SetMallocSpin(bool on) {
  g_malloc_spin.store(on, std::memory_order_relaxed);
}
void SetMallocSpinFactor(int factor) {
  g_malloc_spin_factor.store(factor, std::memory_order_relaxed);
}
}  // namespace bo2zm

#endif  // __SWITCH__
