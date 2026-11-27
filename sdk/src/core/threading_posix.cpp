/**
 * @file        core/threading_posix.cpp
 * @brief       POSIX platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread_audio.h>
#include <rex/cvar.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>
#include <sys/mman.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <ctime>
#include <deque>
#include <exception>
#include <limits>
#include <memory>

#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <semaphore.h>

#if defined(__APPLE__)
#include <cstdio>

#include <fcntl.h>
#endif
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging.h>
#include <rex/thread/timer_queue.h>

#include <sched.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/string.h>
#endif

#if REX_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define REX_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define REX_HAS_SIGEV_THREAD_ID 0
#endif

REXCVAR_DEFINE_BOOL(mac_precise_sleep, false, "Kernel",
                    "macOS: guest timed sleeps wait on a NOTE_CRITICAL kqueue timer, which macOS does not "
                    "coalesce (a 1 ms nanosleep otherwise wakes 1 to 9 ms late)");

REXCVAR_DEFINE_STRING(audio_thread_policy, "realtime", "Audio",
                      "How the threads that produce sound (Audio Worker, XMA Decoder, and the guest threads named in "
                      "audio_guest_threads) are scheduled on macOS. realtime = Mach time-constraint policy, as Core "
                      "Audio's own I/O thread: they run within a millisecond of waking even with every core busy. "
                      "fifo = fixed priority 47 (pthread SCHED_FIFO). off = the user-interactive QoS class every "
                      "thread has, which on a loaded machine left 30-47 % of the blocks late");

namespace rex::thread {

std::string SetThreadAudioPriority(Thread* thread) {
#if defined(__APPLE__)
  const pthread_t target = thread ? reinterpret_cast<pthread_t>(thread->native_handle()) : pthread_self();
  const std::string policy = REXCVAR_GET(audio_thread_policy);
  if (policy == "realtime") {
    // One 256-sample block at 48 kHz is 5.33 ms: the thread asks for that period, may compute for up to 2 ms of it
    // and must finish within the period. A thread that keeps overrunning its computation is demoted by the kernel
    // to the default policy (the fail-safe), so a spinning guest thread cannot take a core for good.
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const double abs_per_ms = 1e6 * double(timebase.denom) / double(timebase.numer);
    thread_time_constraint_policy_data_t constraint{};
    constraint.period = uint32_t(5.333 * abs_per_ms);
    constraint.computation = uint32_t(2.0 * abs_per_ms);
    constraint.constraint = uint32_t(5.333 * abs_per_ms);
    constraint.preemptible = 1;
    const kern_return_t result =
        thread_policy_set(pthread_mach_thread_np(target), THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&constraint), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    return result == KERN_SUCCESS ? std::string("real-time (time constraint 5.33 ms, 2 ms computation)")
                                  : "real-time REFUSED (" + std::to_string(result) + ")";
  }
  if (policy == "fifo") {
    sched_param param{};
    param.sched_priority = 47;
    const int result = pthread_setschedparam(target, SCHED_FIFO, &param);
    return result == 0 ? std::string("fixed priority 47 (SCHED_FIFO)")
                       : "SCHED_FIFO 47 REFUSED (" + std::to_string(result) + ")";
  }
  return "unchanged (audio_thread_policy = " + policy + ")";
#else
  (void)thread;
  return "unchanged (no audio scheduling class on this platform)";
#endif
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf, size_t n);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ = reinterpret_cast<decltype(android_pthread_getname_np_)>(
          dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Thread interruption is done using user-defined signals
// This implementation uses the SIGRTMAX - SIGRTMIN to signal to a thread
// gdb tip, for SIG = SIGRTMIN + SignalType : handle SIG nostop
// lldb tip, for SIG = SIGRTMIN + SignalType : process handle SIG -s false
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
#if REX_PLATFORM_ANDROID
  // pthread_cancel is not available on Android, using a signal handler for
  // simplified PTHREAD_CANCEL_ASYNCHRONOUS-like behavior - not disabling
  // cancellation currently, so should be enough.
  kThreadTerminate,
#endif
  k_Count
};

int GetSystemSignal(SignalType num) {
#if defined(__APPLE__)
  // macOS has no realtime signals (SIGRTMIN..SIGRTMAX); map the runtime's
  // signals onto the two user-defined signals.
  switch (num) {
    case SignalType::kThreadSuspend:
      return SIGUSR1;
    case SignalType::kThreadUserCallback:
      return SIGUSR2;
    default:
      assert_unhandled_case(num);
      return SIGUSR1;
  }
#else
  auto result = SIGRTMIN + static_cast<int>(num);
  assert_true(result < SIGRTMAX);
  return result;
#endif
}

SignalType GetSystemSignalType(int num) {
#if defined(__APPLE__)
  switch (num) {
    case SIGUSR1:
      return SignalType::kThreadSuspend;
    case SIGUSR2:
      return SignalType::kThreadUserCallback;
    default:
      assert_unhandled_case(num);
      return SignalType::kThreadSuspend;
  }
#else
  return static_cast<SignalType>(num - SIGRTMIN);
#endif
}

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)> signal_handler_installed =
    {};

static void signal_handler(int signal, siginfo_t* info, void* context);

// Blocks the suspend and user-callback signals on the calling thread. Called right before a thread
// exits: from pthread_exit onward the thread tears down its thread-local storage, and a signal landing
// then runs signal_handler while libmalloc is mid-free. Any thread_local access in the handler can
// re-instantiate the TLV block (a malloc), which aborts with "Trying to recursively lock an
// os_unfair_lock". Seen at quit, when TerminateTitle wakes every guest thread while some are exiting.
// A blocked signal still pending when the thread dies is discarded.
static void block_thread_signals_for_exit() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, GetSystemSignal(SignalType::kThreadSuspend));
  sigaddset(&set, GetSystemSignal(SignalType::kThreadUserCallback));
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void install_signal_handler(SignalType type) {
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)].compare_exchange_strong(expected,
                                                                                   true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
#if defined(__APPLE__)
  return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

void MaybeYield() {
  SpinScope spin;
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

namespace {
// -1: not yet read from the cvar.
std::atomic<int> precise_sleep_{-1};
}  // namespace

bool PreciseSleepEnabled() {
#if defined(__APPLE__)
  int v = precise_sleep_.load(std::memory_order_relaxed);
  if (v < 0) {
    int expected = -1;
    precise_sleep_.compare_exchange_strong(expected, REXCVAR_GET(mac_precise_sleep) ? 1 : 0);
    v = precise_sleep_.load(std::memory_order_relaxed);
  }
  return v == 1;
#else
  return false;
#endif
}

void SetPreciseSleep(bool on) {
#if defined(__APPLE__)
  precise_sleep_.store(on ? 1 : 0, std::memory_order_relaxed);
  REXLOG_INFO("[precise-sleep] {}", on ? "on" : "off");
#else
  (void)on;
#endif
}

#if defined(__APPLE__)
namespace {
// One kqueue per thread, closed at thread exit.
struct ThreadKqueue {
  int fd = -1;
  ~ThreadKqueue() {
    if (fd >= 0) close(fd);
  }
};
thread_local ThreadKqueue thread_kqueue_;

// Waits until steady deadline on a one-shot NOTE_CRITICAL timer; false if a kqueue is unavailable.
bool CriticalSleepUntil(std::chrono::steady_clock::time_point deadline) {
  if (thread_kqueue_.fd < 0) {
    thread_kqueue_.fd = kqueue();
    if (thread_kqueue_.fd < 0) return false;
  }
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return true;
    const int64_t left_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    struct kevent64_s ev;
    EV_SET64(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, left_ns, 0, 0, 0);
    struct kevent64_s out;
    const int n = kevent64(thread_kqueue_.fd, &ev, 1, &out, 1, 0, nullptr);
    if (n < 0 && errno != EINTR) return false;
  }
}
}  // namespace
#endif

void Sleep(std::chrono::microseconds duration) {
  SpinScope spin;  // wait mechanics count as spin in the deterministic bench
#if defined(__APPLE__)
  if (duration.count() > 0 && PreciseSleepEnabled() &&
      CriticalSleepUntil(std::chrono::steady_clock::now() + duration)) {
    return;
  }
#endif
  timespec rqtp = DurationToTimeSpec(duration);
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

#if defined(__APPLE__)
// macOS does not implement unnamed POSIX semaphores (sem_init fails with
// ENOSYS). Emulate one with a named semaphore that is unlinked immediately
// after creation: the name is released but the descriptor stays valid until
// sem_close, and sem_wait/sem_post remain async-signal-safe (needed because
// WaitSuspended() runs from a signal handler).
static sem_t* RexCreateAnonymousSemaphore() {
  static std::atomic<uint64_t> counter{0};
  char name[64];
  std::snprintf(name, sizeof(name), "/rexsusp_%d_%llu", static_cast<int>(getpid()),
                static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)));
  sem_t* sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
  if (sem != SEM_FAILED) {
    sem_unlink(name);
  }
  return sem;
}
#endif  // __APPLE__


// How often an alertable wait checks for queued user callbacks.
constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if REX_PLATFORM_LINUX
    // Use robust mutexes so waits can recover if owner thread terminates.
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
      if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0) {
        auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
        pthread_mutex_destroy(native_mutex);
        pthread_mutex_init(native_mutex, &attr);
      }
      pthread_mutexattr_destroy(&attr);
    }
#endif
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    return WaitUntil(DeadlineFromTimeout(timeout), nullptr, nullptr);
  }

  static std::chrono::steady_clock::time_point DeadlineFromTimeout(std::chrono::milliseconds timeout) {
    if (timeout == std::chrono::milliseconds::max()) {
      return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + timeout;
  }

  // Waits for this object until `deadline` (time_point::max() = forever).
  //
  // A thread that blocks is queued (WaitNode) in arrival order, and an event's Set or Pulse releases the queued
  // threads directly, as the Xbox 360 / NT kernel does: a released thread succeeds even if the event is reset
  // or taken by another thread before it runs again, and a thread that arrives after the Set cannot take the
  // signal from one that was already waiting.
  //
  // `to_signal` (SignalAndWait): signaled only once this thread is queued, so a thread it wakes that signals
  // this object back cannot take that signal with its own wait first. The two objects' locks are never held
  // together (SignalAndWait(a, b) and SignalAndWait(b, a) at once must not deadlock).
  //
  // `alert_flag` (alertable waits): kUserCallback as soon as it is set, checked every kAlertablePollSlice while
  // staying queued.
  WaitResult WaitUntil(std::chrono::steady_clock::time_point deadline, PosixConditionBase* to_signal,
                       const std::atomic<bool>* alert_flag) {
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    if (!LockForWait(lock)) {
      return WaitResult::kFailed;
    }
    const bool infinite = deadline == std::chrono::steady_clock::time_point::max();
    WaitNode node;
    bool queued = false;
    if (to_signal) {
      if (!signaled()) {
        Enqueue(&node);
        queued = true;
      }
      lock.unlock();
      const bool signal_ok = to_signal->Signal();
      if (!LockForWait(lock)) {
        // The node is on this stack and may be queued: it cannot be left behind. Only a mutex that is no longer
        // usable (ENOTRECOVERABLE) gets here.
        std::terminate();
      }
      if (!signal_ok) {
        if (node.released) {
          ReturnRelease();
        } else if (queued) {
          Unlink(&node);
        }
        return WaitResult::kFailed;
      }
    }
    while (true) {
      if (node.released) {
        return WaitResult::kSuccess;  // The releaser unlinked the node.
      }
      if (signaled()) {
        if (queued) {
          Unlink(&node);
        }
        post_execution();
        return WaitResult::kSuccess;
      }
      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
        if (queued) {
          Unlink(&node);
        }
        return WaitResult::kUserCallback;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!infinite && now >= deadline) {
        if (queued) {
          Unlink(&node);
        }
        return WaitResult::kTimeout;
      }
      if (!queued) {
        Enqueue(&node);
        queued = true;
      }
      if (alert_flag) {
        const auto slice_end = now + kAlertablePollSlice;
        cond_.wait_until(lock, infinite ? slice_end : std::min(deadline, slice_end));
      } else if (infinite) {
        cond_.wait(lock);
      } else {
        cond_.wait_until(lock, deadline);
      }
    }
  }

  static std::pair<WaitResult, size_t> WaitMultiple(PosixConditionBase* const* handles,
                                                    size_t count, bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    assert_true(count != 0);

    if (count == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    constexpr size_t kInlineLocks = 64;
    std::unique_lock<std::mutex> inline_locks[kInlineLocks];
    std::unique_ptr<std::unique_lock<std::mutex>[]> heap_locks;
    auto* locks = inline_locks;
    if (count > kInlineLocks) {
      heap_locks = std::make_unique<std::unique_lock<std::mutex>[]>(count);
      locks = heap_locks.get();
    }
    size_t lock_count = 0;
    auto clear_locks = [&]() {
      while (lock_count) {
        locks[--lock_count] = std::unique_lock<std::mutex>();
      }
    };
    while (true) {
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;
      bool all_locked = true;

      for (size_t i = 0; i < count; ++i) {
#if REX_PLATFORM_LINUX
        auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks[lock_count++] = std::unique_lock<std::mutex>(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#else
        locks[lock_count] = std::unique_lock<std::mutex>(handles[i]->mutex_, std::try_to_lock);
        if (!locks[lock_count++].owns_lock()) {
          all_locked = false;
          break;
        }
#endif
      }

      if (!all_locked) {
        clear_locks();
        // The timeout also expires when an object cannot be inspected.
        // The earlier continue skipped the only timeout check.
        const auto now = std::chrono::steady_clock::now();
        if (now >= end_time) {
          return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
        }
        MaybeYield();
        continue;
      }

      if (wait_all) {
        bool all_signaled = true;
        for (size_t i = 0; i < count; ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        for (size_t i = 0; i < count; ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        if (wait_all) {
          for (size_t i = 0; i < count; ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      clear_locks();

      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      if (timeout == std::chrono::milliseconds::max()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        // In microseconds. In milliseconds, the remainder of a short wait was
        // truncated to 0 and sleep_for(0) does not sleep: the alertable wait asks
        // for 1 ms slices, so WaitAny spun for its whole timeout. On Switch the
        // audio thread (0x2B) ate a whole core this way.
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(end_time - now);
        auto sleep_time = std::min(remaining, std::chrono::microseconds(1000));
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

  bool CaptureRewindState(WaitHandleRewindState* state) {
    state->word = CaptureRewindValue();
    state->auxiliary = CaptureRewindAuxiliary();
    state->owner = CaptureRewindOwner();
    state->waiters = waiters_;
    return true;
  }
  void Rewind(const WaitHandleRewindState& state) {
    RestoreRewindValue(state.word);
    RestoreRewindAuxiliary(state.auxiliary);
    RestoreRewindOwner(state.owner);
    waiters_ = state.waiters;
  }

 protected:
  virtual uint64_t CaptureRewindValue() const = 0;
  virtual void RestoreRewindValue(uint64_t value) = 0;
  virtual uint64_t CaptureRewindAuxiliary() const { return 0; }
  virtual void RestoreRewindAuxiliary(uint64_t) {}
  virtual uintptr_t CaptureRewindOwner() const { return 0; }
  virtual void RestoreRewindOwner(uintptr_t) {}

  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;

  // A thread blocked in WaitUntil, queued in arrival order (under mutex_).
  struct WaitNode {
    WaitNode* prev = nullptr;
    WaitNode* next = nullptr;
    bool released = false;  // Set by ReleaseOldestWaiter: the wait has succeeded.
  };

  // Takes mutex_ for a wait. On Linux the mutex is robust: a thread that died holding it does not block waits
  // forever.
  bool LockForWait(std::unique_lock<std::mutex>& lock) {
#if REX_PLATFORM_LINUX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return false;
    }
    lock = std::unique_lock<std::mutex>(mutex_, std::adopt_lock);
#else
    // On Horizon a plain blocking lock. The earlier try_lock + 100 us sleep loop (so that the timeout also
    // covered taking the lock) was most of the samples of BO2's job worker threads in a console profile, and
    // every sleep delayed the wake-up. This mutex is only held for a few instructions, and Horizon's kernel
    // mutex lends the holder the waiter's priority, so a blocked waiter cannot starve it.
    lock.lock();
#endif
    return true;
  }

  void Enqueue(WaitNode* node) {
    node->prev = wait_tail_;
    node->next = nullptr;
    if (wait_tail_) {
      wait_tail_->next = node;
    } else {
      wait_head_ = node;
    }
    wait_tail_ = node;
    ++waiters_;
  }
  void Unlink(WaitNode* node) {
    (node->prev ? node->prev->next : wait_head_) = node->next;
    (node->next ? node->next->prev : wait_tail_) = node->prev;
    node->prev = node->next = nullptr;
    --waiters_;
  }
  // Ends the wait of the thread that has waited longest. The caller notifies cond_. False if nobody waits.
  bool ReleaseOldestWaiter() {
    WaitNode* node = wait_head_;
    if (!node) {
      return false;
    }
    Unlink(node);
    node->released = true;
    return true;
  }
  void ReleaseAllWaiters() {
    while (ReleaseOldestWaiter()) {
    }
  }
  // SignalAndWait's signal failed after this thread had been released: give the release back (under mutex_).
  virtual void ReturnRelease() {}

  // Threads blocked in WaitUntil (the length of the wait_head_ list). Under mutex_.
  uint32_t waiters_ = 0;
  WaitNode* wait_head_ = nullptr;
  WaitNode* wait_tail_ = nullptr;
  std::condition_variable cond_;
  std::mutex mutex_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  // SetEvent. A manual-reset event releases every thread waiting now (they succeed even if the event is reset
  // before they run, the Set + Reset pulse that guest condition variables use) and stays signaled. An auto-reset
  // event hands the signal to the thread that has waited longest and stays non-signaled; with nobody waiting it
  // stays signaled until a wait takes it.
  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (manual_reset_) {
      signal_ = true;
      ReleaseAllWaiters();
    } else if (!ReleaseOldestWaiter()) {
      signal_ = true;
    }
    cond_.notify_all();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = false;
  }

  // PulseEvent. Releases the threads waiting right now and leaves the event non-signaled: a
  // manual-reset event wakes them all, an auto-reset one wakes one (it takes the signal, as with SetEvent). With
  // nobody waiting the pulse is lost, as on Windows. It replaces Signal + yield + Sleep(10 us) + Reset, which
  // the game's main and server threads paid on every pulse (BO2 pulses its job event for each batch of jobs).
  void Pulse() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (manual_reset_) {
      ReleaseAllWaiters();
    } else {
      ReleaseOldestWaiter();
    }
    signal_ = false;
    cond_.notify_all();
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  uint64_t CaptureRewindValue() const override { return signal_; }
  void RestoreRewindValue(uint64_t value) override { signal_ = value != 0; }
  void ReturnRelease() override {
    if (!manual_reset_) {
      if (!ReleaseOldestWaiter()) {
        signal_ = true;
      }
      cond_.notify_all();
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    cond_.notify_all();
    return true;
  }

 private:
  inline bool signaled() const override { return count_ > 0; }
  inline void post_execution() override {
    count_--;
    cond_.notify_all();
  }
  uint64_t CaptureRewindValue() const override { return count_; }
  void RestoreRewindValue(uint64_t value) override { count_ = uint32_t(value); }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        cond_.notify_all();
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint64_t CaptureRewindValue() const override { return count_; }
  void RestoreRewindValue(uint64_t value) override { count_ = uint32_t(value); }
  uintptr_t CaptureRewindOwner() const override {
    uintptr_t value = 0;
    static_assert(sizeof(owner_) <= sizeof(value));
    std::memcpy(&value, &owner_, sizeof(owner_));
    return value;
  }
  void RestoreRewindOwner(uintptr_t value) override {
    static_assert(sizeof(owner_) <= sizeof(value));
    std::memcpy(&owner_, &value, sizeof(owner_));
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  uint64_t CaptureRewindValue() const override { return signal_; }
  void RestoreRewindValue(uint64_t value) override { signal_ = value != 0; }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};


struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
#if defined(__APPLE__)
    suspend_sem_ = RexCreateAnonymousSemaphore();
#else
    sem_init(&suspend_sem_, 0, 0);
#endif
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return false;
    if (pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    if (params.initial_priority != 0) {
      sched_param sched{};
      sched.sched_priority = params.initial_priority + 1;
      if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
      if (pthread_attr_setschedparam(&attr, &sched) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
    }
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
#if defined(__APPLE__)
    suspend_sem_ = RexCreateAnonymousSemaphore();
#else
    sem_init(&suspend_sem_, 0, 0);
#endif
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  virtual ~PosixCondition() {
    // Match Canary/Edge behavior.
    // Force-cancel/join from the condition destructor can self-join/crash
    // depending on shutdown ordering, so threads must be stopped explicitly.
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(), result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        rex::string::copy_truncating(result.data(), android_pre_api_26_name_, result.size());
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if defined(__APPLE__)
      // macOS pthread_setname_np() only names the calling thread.
      if (pthread_equal(thread_, pthread_self())) {
        pthread_setname_np(std::string(name).c_str());
      }
#else
      pthread_setname_np(thread_, std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
#endif
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::string::copy_truncating(android_pre_api_26_name_, name,
                                 rex::countof(android_pre_api_26_name_));
  }
#endif

#if defined(__APPLE__)
  uint32_t system_id() const { return static_cast<uint32_t>(pthread_mach_thread_np(thread_)); }
#else
  uint32_t system_id() const { return static_cast<uint32_t>(thread_); }
#endif

  uint64_t affinity_mask() {
    WaitStarted();
#if defined(__APPLE__)
    // macOS doesn't expose Linux-style pthread affinity masks; report every
    // logical processor as available.
    auto cpu_count = std::min<uint32_t>(logical_processor_count(), 64);
    if (cpu_count == 64) {
      return std::numeric_limits<uint64_t>::max();
    }
    return (uint64_t(1) << cpu_count) - 1;
#else
    cpu_set_t cpu_set;
#if REX_PLATFORM_ANDROID
    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
    uint64_t result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (auto i = 0u; i < cpu_count; i++) {
      auto set = CPU_ISSET(i, &cpu_set);
      result |= set << i;
    }
    return result;
#endif
  }

  void set_affinity_mask(uint64_t mask) {
    WaitStarted();
#if defined(__APPLE__)
    // macOS doesn't expose Linux-style pthread affinity masks; keep this a
    // no-op so higher layers can build and run.
    (void)mask;
#else
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (1ULL << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if REX_PLATFORM_ANDROID
    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
#endif
  }

  int priority() {
    WaitStarted();
    int policy;
    sched_param param{};
    int ret = pthread_getschedparam(thread_, &policy, &param);
    if (ret != 0) {
      return -1;
    }

    return param.sched_priority;
  }

  void set_priority(int new_priority) {
    WaitStarted();
#if defined(__APPLE__)
    // pthread_setschedparam permanently opts a thread out of QoS (qos.h), undoing the user-interactive class
    // ThreadStartRoutine set, so macOS could park the thread on efficiency cores. Xbox priorities 0..2 are not
    // even in Darwin's SCHED_FIFO range (15..47); keep the QoS class instead.
    (void)new_priority;
    return;
#endif
    sched_param param{};
    param.sched_priority = new_priority;
    int result = pthread_setschedparam(thread_, SCHED_FIFO, &param);
    if (result != 0) {
      switch (result) {
        case EPERM:
          REXSYS_WARN("set_priority: permission denied");
          break;
        case EINVAL:
          assert_always();
          break;
        default:
          REXSYS_WARN("set_priority: pthread_setschedparam failed ({})", result);
          break;
      }
    }
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    // If the callback is queued on the current thread, don't self-signal.
    // Alertable waits drain this queue in normal thread context.
    if (pthread_equal(thread_, pthread_self())) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

    // A thread that has finished (returned, or is inside Terminate on its way to pthread_exit) never
    // runs another callback. Signalling it only risks landing in its teardown, and once it is gone
    // pthread_kill fails with ESRCH ("signal delivery failed (3)" at every quit).
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        return;
      }
    }

#if defined(__APPLE__)
    // macOS has no sigqueue/pthread_sigqueue; pthread_kill delivers the signal
    // without the sigval payload.
    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadUserCallback));
#else
    sigval value{};
    value.sival_ptr = this;
#if REX_PLATFORM_ANDROID
    int result = sigqueue(pthread_gettid_np(thread_),
                          GetSystemSignal(SignalType::kThreadUserCallback), value);
#else
    int result = pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback), value);
#endif
#endif
    if (result != 0) {
      REXSYS_WARN("QueueUserCallback: signal delivery failed ({})", result);
    }
  }

  // Set while user callbacks are queued; an alertable wait ends when it is set.
  const std::atomic<bool>* pending_user_callbacks_flag() const { return &has_pending_user_callbacks_; }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Async-signal-safe wakeup for WaitSuspended() from signal handler path.
      sem_post(suspend_sem_ptr());
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    bool already_suspended;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      already_suspended = suspend_count_ > 0;
      state_ = State::kSuspended;
      ++suspend_count_;
    }
    // Nested suspend: the thread is already parked (or its signal is pending). A second signal would stay
    // pending and park it again right after the matching Resume, with nobody left to wake it.
    if (already_suspended) {
      return true;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // This is really bad. Some thread must have called Terminate() on us
          // just before we decided to terminate ourselves
          assert_always();
          for (;;) {
            // Wait for pthread_cancel() to actually happen.
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
    }
    if (is_current_thread) {
      block_thread_signals_for_exit();
      pthread_exit(reinterpret_cast<void*>(exit_code));
    } else {
#if REX_PLATFORM_ANDROID
      if (pthread_kill(thread_, GetSystemSignal(SignalType::kThreadTerminate)) != 0) {
        assert_always();
      }
#else
      if (pthread_cancel(thread_) != 0) {
        assert_always();
      }
#endif
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  // ucontext_t* of the interrupted state while this thread is parked in the suspend signal handler;
  // null otherwise. Used by the in-process savepoint (GetSuspendedContext).
  std::atomic<void*> suspend_uc_{nullptr};

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(suspend_sem_ptr());
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }


 private:
  static void* ThreadStartRoutine(void* parameter);
  uint64_t CaptureRewindValue() const override { return signaled_; }
  void RestoreRewindValue(uint64_t value) override { signaled_ = value != 0; }
  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
    if (thread_) {
      pthread_join(thread_, nullptr);
    }
#if defined(__APPLE__)
    if (suspend_sem_ != SEM_FAILED) {
      sem_close(suspend_sem_);
      suspend_sem_ = SEM_FAILED;
    }
#else
    sem_destroy(&suspend_sem_);
#endif
  }
  pthread_t thread_;
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
#if defined(__APPLE__)
  sem_t* suspend_sem_ = SEM_FAILED;
  sem_t* suspend_sem_ptr() { return suspend_sem_; }
#else
  sem_t suspend_sem_;
  sem_t* suspend_sem_ptr() { return &suspend_sem_; }
#endif
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
#if REX_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

const std::atomic<bool>* CurrentThreadPendingUserCallbacksFlag() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ ? current_thread_condition_->pending_user_callbacks_flag() : nullptr;
}


namespace {

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds ComputeAlertableWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return kAlertablePollSlice;
  }
  // A positive remainder under 1 ms must keep waiting. Truncating it to zero
  // turns the end of every alertable wait into a busy poll on Horizon.
  auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return std::min(remaining, kAlertablePollSlice);
}

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread) : handle_(thread) {}

bool CaptureWaitHandleRewindState(WaitHandle* handle, WaitHandleRewindState* state) {
  auto* posix_handle = dynamic_cast<PosixWaitHandle*>(handle);
  return posix_handle && posix_handle->condition().CaptureRewindState(state);
}

void RewindWaitHandle(WaitHandle* handle, const WaitHandleRewindState& state) {
  if (auto* posix_handle = dynamic_cast<PosixWaitHandle*>(handle)) {
    posix_handle->condition().Rewind(state);
  }
}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  SpinScope spin;  // wait mechanics count as spin in the deterministic bench
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  auto& condition = posix_wait_handle->condition();
  if (!is_alertable) {
    return condition.Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  const auto deadline = ComputeAlertableDeadline(timeout);
  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    // Stays queued on the object for the whole wait (a signal meant for this thread cannot go to a later
    // arrival between polls) and returns kUserCallback once a callback is queued.
    const auto result = condition.WaitUntil(deadline, nullptr, alert_flag);
    if (result != WaitResult::kUserCallback) {
      return result;
    }
  }
}

// NtSignalAndWaitForSingleObjectEx: the signal and the start of the wait are one operation (see
// PosixConditionBase::WaitUntil), and the object is signaled exactly once whatever the wait returns.
WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  auto& to_signal = posix_wait_handle_to_signal->condition();
  auto& to_wait_on = posix_wait_handle_to_wait_on->condition();
  if (&to_signal == &to_wait_on) {
    if (!to_signal.Signal()) {
      return WaitResult::kFailed;
    }
    return Wait(wait_handle_to_wait_on, is_alertable, timeout);
  }
  const auto deadline = ComputeAlertableDeadline(timeout);
  if (!is_alertable) {
    return to_wait_on.WaitUntil(deadline, &to_signal, nullptr);
  }

  ScopedAlertableState alertable_state_guard(true);
  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
  if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
    // A callback is already queued: signal, run it and return, as the alertable wait would at once.
    if (!to_signal.Signal()) {
      return WaitResult::kFailed;
    }
    DispatchCurrentThreadUserCallback();
    return WaitResult::kUserCallback;
  }
  PosixConditionBase* pending_signal = &to_signal;
  while (true) {
    const auto result = to_wait_on.WaitUntil(deadline, pending_signal, alert_flag);
    pending_signal = nullptr;
    if (result != WaitResult::kUserCallback) {
      return result;
    }
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  SpinScope spin;  // wait mechanics count as spin in the deterministic bench
  constexpr size_t kInlineConditions = 64;
  PosixConditionBase* inline_conditions[kInlineConditions];
  std::unique_ptr<PosixConditionBase*[]> heap_conditions;
  auto** conditions = inline_conditions;
  if (wait_handle_count > kInlineConditions) {
    heap_conditions = std::make_unique<PosixConditionBase*[]>(wait_handle_count);
    conditions = heap_conditions.get();
  }
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions[i] = &handle->condition();
  }
  if (wait_handle_count == 1) {
    // One object: the queued wait (stays queued through alertable polls).
    return std::make_pair(Wait(wait_handles[0], is_alertable, timeout), size_t(0));
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(conditions, wait_handle_count, wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(conditions, wait_handle_count, wait_all,
                                                   ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
  }
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override { handle_.Pulse(); }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
  std::atomic<void*>& handle_suspend_uc() { return handle_.suspend_uc_; }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    assert_always();
  }
#endif
  set_current_thread_name("");
#if defined(__APPLE__)
  {
    // The suspend signal handler runs on an alternate stack so the interrupted thread's own stack stays
    // untouched while parked (the in-process savepoint rewrites it).
    constexpr size_t kAltStack = 128 * 1024;
    void* mem = mmap(nullptr, kAltStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem != MAP_FAILED) {
      stack_t ss{};
      ss.ss_sp = mem;
      ss.ss_size = kAltStack;
      ss.ss_flags = 0;
      sigaltstack(&ss, nullptr);
    }
  }
#endif

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
#if defined(__APPLE__)
  // Guest threads (game, render, audio) at the user-interactive QoS class, as the main thread already is: the
  // default class lets macOS move them to efficiency cores and delay their wake-ups. Only a thread itself can set
  // its class once it runs.
  if (const int qos_result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); qos_result != 0) {
    REXSYS_WARN("guest thread QoS: user-interactive refused ({})", qos_result);
  }
#endif
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    // Arm the suspend counter in the SAME critical section that publishes the
    // started state. Resume() only waits for state_ != kUninitialized, so if it
    // observed the state published here while suspend_count_ was still 0 it
    // would take the "not suspended" path, return false without decrementing,
    // and the wait below would then never be satisfied.
    if (create_suspended) {
      thread->handle_.suspend_count_ = 1;
    }
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_signal_.wait(lock,
                                       [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.cond_.notify_all();
  }

  block_thread_signals_for_exit();
  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Threads not created by Thread::Create (typically main thread) still need
  // process-wide signal handlers used by suspend/APC machinery.
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  current_thread_condition_ = &current_thread_->condition();
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void* GetSuspendedContext(Thread* thread) {
  auto* pt = dynamic_cast<PosixThread*>(thread);
  return pt ? pt->handle_suspend_uc().load(std::memory_order_acquire) : nullptr;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    block_thread_signals_for_exit();
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
#if defined(__APPLE__)
  pthread_setname_np(std::string(name).c_str());
#else
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
}

// Kept out of line so the compiler cannot hoist the thread_local access above the signal type check
// in signal_handler: it did, so every user-callback wakeup touched the TLV (see
// block_thread_signals_for_exit for why that can abort).
[[gnu::noinline]] static void suspend_current_thread_from_signal(void* context) {
  PosixThread* thread = current_thread_;
  if (thread) {
    thread->handle_suspend_uc().store(context, std::memory_order_release);
    thread->WaitSuspended();
    thread->handle_suspend_uc().store(nullptr, std::memory_order_release);
  }
}

static void signal_handler(int signal, siginfo_t* /*info*/, void* context) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      suspend_current_thread_from_signal(context);
    } break;
    case SignalType::kThreadUserCallback: {
      // Callbacks are drained from alertable waits in normal thread context.
      // This signal is only used as a wakeup hint.
    } break;
#if REX_PLATFORM_ANDROID
    case SignalType::kThreadTerminate: {
      pthread_exit(reinterpret_cast<void*>(-1));
    } break;
#endif
    default:
      assert_always();
  }
}

}  // namespace rex::thread
