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
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_switch.h>
#include <mach/mach_traps.h>
#endif

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

// On Horizon the kernel objects (events, semaphores, mutants, timers, thread exit) wait and signal
// through the address arbiter instead of a mutex and a condition variable per object; see the comment above the
// REX_FAST_WAITS PosixConditionBase. REX_FAST_WAITS_TEST builds the same code on Linux (futex) for the stress test
// in this port's tools/runtime_stress. Every other POSIX build keeps the original implementation.
#if defined(__SWITCH__) || defined(REX_FAST_WAITS_TEST)
#define REX_FAST_WAITS 1
#else
#define REX_FAST_WAITS 0
#endif

#include <signal.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <ctime>
#include <deque>
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
#if defined(__SWITCH__)
// Horizon has no sys/syscall.h. The shim provides affinity, priority, cpu_set_t
// and the thread id. See core/threading_switch.h.
#include "threading_switch.h"
#else
#include <sys/syscall.h>
#endif
#if REX_FAST_WAITS && !defined(__SWITCH__)
#include <linux/futex.h>
#endif
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
#if defined(__APPLE__) || defined(__SWITCH__)
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
#if defined(__APPLE__) || defined(__SWITCH__)
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
#if !defined(__SWITCH__)
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, GetSystemSignal(SignalType::kThreadSuspend));
  sigaddset(&set, GetSystemSignal(SignalType::kThreadUserCallback));
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif
}

void install_signal_handler(SignalType type) {
#if defined(__SWITCH__)
  // Horizon does not deliver signals to user processes. Installing the handler
  // would not fail, it would simply never be called, and that is worse than not
  // doing it: it would suggest that signal-based suspension works.
  //
  // Real suspension goes through svcSetThreadActivity, in Suspend()/Resume().
  // See core/threading_switch.h.
  (void)type;
#else
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)].compare_exchange_strong(expected,
                                                                                   true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
#endif
}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
#if defined(__APPLE__)
  return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#elif defined(__SWITCH__)
  return RexSwitchCurrentThreadId();
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

REXCVAR_DEFINE_STRING(thread_yield, "switch", "Kernel",
                      "How a spinning thread gives way (NtYieldExecution, spin locks, zero-timeout polls, the GPU "
                      "ring's register waits). switch = macOS thread_switch with no priority change: lets another "
                      "thread run if one is waiting for this core, about 2 us when none is. sched_yield = as before: on "
                      "macOS that also drops the thread's priority for a whole quantum, so with the machine busy a "
                      "game thread polling for work came back 10 to 130 ms later. Elsewhere both are sched_yield")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
#if defined(__APPLE__)
bool YieldBySwitch() {
  static const bool on = REXCVAR_GET(thread_yield) != "sched_yield";
  return on;
}
#endif
}  // namespace

void MaybeYield() {
#if defined(__APPLE__)
  // sched_yield on macOS is swtch_pri(0): it depresses the thread to priority 0 for a quantum (10 ms), so
  // every niced build and every other process runs first. thread_switch with SWITCH_OPTION_NONE hands the
  // core to a waiting thread of the same or higher priority, if there is one, and keeps the priority.
  if (YieldBySwitch()) {
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
  } else {
    sched_yield();
  }
#else
  sched_yield();
#endif
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void Sleep(std::chrono::microseconds duration) {
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
#if !REX_FAST_WAITS  // the REX_FAST_WAITS version sleeps on the thread's wake word; it is further down
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
#endif  // !REX_FAST_WAITS

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

#if REX_FAST_WAITS
// ===================================================================================================================
// Waits and signals on the address arbiter.
//
// Why: in a console profile the main thread spent 12-27 % of its samples blocked on one event's mutex inside
// PulseEvent (NtPulseEvent from BO2's job system, sub_8284A910 via sub_82492D48), and the job workers sat on that
// same mutex and condition variable in Wait. With about ten busy threads at the
// time-sliced priority 0x3B on three cores, the thread holding the mutex was often not running, and every pulse and
// every wake-up queued behind it.
//
// How: each object keeps its whole state in one 64-bit atomic word.
//   bits  0..31  the value the kernel compares. Event: bit 0 = signaled, bits 1..31 = a generation that Set and
//                Pulse advance on manual-reset events, so a thread released by them is released even if the event
//                is reset again before it runs. Semaphore: the count. Mutant: the recursion count. Thread: exited.
//   bits 32..47  threads sleeping on this object in a single-object, non-alertable wait.
//   bit  48      threads waiting on it in WaitMultiple or in an alertable wait (they sleep on their own word).
// Set, Pulse, Release and Reset are one compare-and-swap, and call the kernel (svcSignalToAddress) only when the
// old word shows a waiter. A single-object waiter registers in the same compare-and-swap that finds the object not
// signaled, then sleeps with svcWaitForAddress(WaitIfEqual) on the low 32 bits; if the word changed in between, the
// kernel returns at once. So the decision to sleep and the decision to wake are each one atomic operation on the
// same word, with no lock (platform-notes.md, Threads).
// Multi-object and alertable waits sleep on a word of their own thread (PosixCondition<Thread>::wake_word_). They
// put a WaitBlock on each object's list (a small mutex that signalers take only when bit 48 is set); a signaler
// bumps the waiter's word and wakes it. QueueUserCallback bumps it too, so an APC ends an alertable wait at once.
// No wait sleeps longer than kWakeCheckInterval at a time. A waiter that then finds its object signaled without
// having been woken reports a lost wake-up in the log (and takes the object); waits longer than kLongWait are
// logged once. A lost wake-up therefore costs at most a second and shows in the log, instead of hanging.
// ===================================================================================================================

#if defined(__SWITCH__)
// Not from switch.h: libnx's global Thread/Event/Timer names clash with rex::thread's (threading_switch.h).
// The same prototypes as libnx's svc.h.
extern "C" uint32_t svcWaitForAddress(void* address, uint32_t arb_type, int64_t value, int64_t timeout);
extern "C" uint32_t svcSignalToAddress(void* address, uint32_t signal_type, int32_t value, int32_t count);
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the kernel compares the low half of the state word, at its own address: little-endian only"
#endif
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));

namespace {

enum class AddressWait { kChanged, kTimeout };

// Sleeps while the 32-bit word at address equals expected, for at most timeout_ns (> 0). kChanged covers being
// woken, a value that already differed and spurious returns: callers always re-read their state.
AddressWait WaitOnAddress(void* address, uint32_t expected, int64_t timeout_ns) {
#if defined(__SWITCH__)
  constexpr uint32_t kArbitrationTypeWaitIfEqual = 2;
  constexpr uint32_t kKernelErrorTimedOut = 117;
  constexpr uint32_t kKernelErrorInvalidState = 125;  // the value already differed
  const uint32_t rc = svcWaitForAddress(address, kArbitrationTypeWaitIfEqual,
                                        int64_t(int32_t(expected)), timeout_ns);
  const uint32_t description = (rc >> 9) & 0x1FFF;
  if (description == kKernelErrorTimedOut) {
    return AddressWait::kTimeout;
  }
  if (rc != 0 && description != kKernelErrorInvalidState) {
    // Anything else would make the callers spin; say so once.
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
      REXSYS_WARN("bo2-zm-nx waits: svcWaitForAddress failed with {:#x}", rc);
    }
  }
  return AddressWait::kChanged;
#else
  timespec ts{static_cast<time_t>(timeout_ns / 1000000000), static_cast<long>(timeout_ns % 1000000000)};
  const long rc = syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
  return (rc == -1 && errno == ETIMEDOUT) ? AddressWait::kTimeout : AddressWait::kChanged;
#endif
}

// Wakes up to count threads sleeping on address (count <= 0: all of them).
void WakeAddress(void* address, int32_t count) {
#if defined(__SWITCH__)
  constexpr uint32_t kSignalTypeSignal = 0;
  svcSignalToAddress(address, kSignalTypeSignal, 0, count);
#else
  syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, count <= 0 ? INT_MAX : count, nullptr, nullptr, 0);
#endif
}

constexpr uint64_t kValueMask = 0xFFFFFFFFull;
constexpr uint64_t kWaiterOne = 1ull << 32;
constexpr uint64_t kWaiterMask = 0xFFFFull << 32;
constexpr uint64_t kMultiFlag = 1ull << 48;
constexpr uint64_t kAnyWaiter = kWaiterMask | kMultiFlag;

inline uint32_t ValueOf(uint64_t state) {
  return static_cast<uint32_t>(state);
}
inline uint64_t WithValue(uint64_t state, uint32_t value) {
  return (state & ~kValueMask) | value;
}

constexpr auto kWakeCheckInterval = std::chrono::seconds(1);
constexpr auto kLongWait = std::chrono::seconds(10);
constexpr uint32_t kWaitLogLimit = 32;  // log lines of each kind per run
std::atomic<uint32_t> lost_wakeup_lines_{0};
std::atomic<uint32_t> long_wait_lines_{0};
std::atomic<uint64_t> lost_wakeups_{0};

// The calling thread's identity, for mutant ownership.
uintptr_t CurrentThreadToken() {
  static thread_local char token;
  return reinterpret_cast<uintptr_t>(&token);
}

// How long to sleep next: up to the deadline, at most kWakeCheckInterval. 0 once the deadline has passed.
int64_t NextSleepNs(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto left = deadline - now;
  const auto slice = left < kWakeCheckInterval
                         ? left
                         : std::chrono::duration_cast<std::chrono::steady_clock::duration>(kWakeCheckInterval);
  return std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(slice).count());
}

std::chrono::steady_clock::time_point DeadlineAfter(std::chrono::steady_clock::time_point start,
                                                    std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max() ||
      timeout > std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::time_point::max() - start)) {
    return std::chrono::steady_clock::time_point::max();
  }
  return start + timeout;
}

int64_t MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
      .count();
}

const std::string& CurrentThreadName() {
  return Thread::GetCurrentThread()->name();
}

void NoteLostWakeup(const char* what, std::chrono::steady_clock::time_point start) {
  const uint64_t count = lost_wakeups_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (lost_wakeup_lines_.fetch_add(1, std::memory_order_relaxed) < kWaitLogLimit) {
    // A real lost wake-up, unless the thread woken instead of this one has not run for a whole check interval.
    REXSYS_WARN(
        "bo2-zm-nx waits: possible lost wake-up #{}: thread '{}' found its {} signaled after {} ms without "
        "being woken (it took it and went on)",
        count, CurrentThreadName(), what, MillisecondsSince(start));
  }
}

void NoteLongWait(const char* what, std::chrono::steady_clock::time_point start, bool* reported) {
  if (*reported || std::chrono::steady_clock::now() - start < kLongWait) {
    return;
  }
  *reported = true;
  if (long_wait_lines_.fetch_add(1, std::memory_order_relaxed) < kWaitLogLimit) {
    REXSYS_INFO("bo2-zm-nx waits: thread '{}' has waited {} ms for {} (still waiting; logged once per wait)",
                CurrentThreadName(), MillisecondsSince(start), what);
  }
}

// A thread waiting in WaitMultiple or alertably, on one object's list.
struct WaitBlock {
  WaitBlock* prev = nullptr;
  WaitBlock* next = nullptr;
  std::atomic<uint32_t>* word = nullptr;  // the waiting thread's wake word
  uint32_t entry = 0;                     // the object's value when the block went in (its generation)
};

std::atomic<uint32_t>* CurrentThreadWakeWord();  // after PosixCondition<Thread>

// WaitAll checks and takes its objects under this lock, so two WaitAll calls cannot each hold part of a set.
std::mutex& WaitAllMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace

// Lost wake-ups found so far (the stress test in app/src/bo2zm_test_waits.cpp reads it).
uint64_t FastWaitLostWakeups() {
  return lost_wakeups_.load(std::memory_order_relaxed);
}

class PosixConditionBase {
 public:
  enum class Kind : uint8_t { kManualEvent, kAutoEvent, kSemaphore, kMutant, kThread };

  explicit PosixConditionBase(Kind kind = Kind::kThread, uint32_t value = 0) : word_(value), kind_(kind) {
    if (kind == Kind::kMutant && value != 0) {
      owner_.store(CurrentThreadToken(), std::memory_order_relaxed);
    }
  }
  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  // A non-alertable wait on this object alone.
  WaitResult Wait(std::chrono::milliseconds timeout);

  // Any (index of the object taken) or all of objects[0..count). Alertable waits also end with kUserCallback
  // when a user callback is queued to the thread.
  static std::pair<WaitResult, size_t> WaitMultiple(PosixConditionBase* const* objects, size_t count,
                                                    bool wait_all, bool alertable,
                                                    std::chrono::milliseconds timeout);

  virtual void* native_handle() const { return const_cast<PosixConditionBase*>(this); }

  // Threads registered as waiting (single-object waits, plus one if any multi-object or alertable wait).
  uint32_t waiters() const {
    const uint64_t state = word_.load(std::memory_order_acquire);
    return uint32_t((state & kWaiterMask) >> 32) + ((state & kMultiFlag) ? 1 : 0);
  }

 protected:
  // After a successful wait by the calling thread (threads: joined once).
  virtual void OnAcquired() {}

  // Events, timers and thread exit.
  void SetSignaled();
  void ResetSignaled() { word_.fetch_and(~uint64_t(1), std::memory_order_acq_rel); }
  void PulseSignaled();
  // Semaphores: adds count unless it would pass maximum.
  bool AddCount(uint32_t count, uint32_t maximum, uint32_t* previous);
  // Mutants: false when the calling thread does not own it.
  bool ReleaseOwnership();

 private:
  bool Satisfiable(uint64_t state, uint32_t entry) const;
  uint32_t Consumed(uint32_t value) const;
  bool TryConsume(uint32_t entry);
  void Acquired();
  void Unconsume();
  void WakeAfter(uint64_t old_state, int32_t single_count);
  void AddBlock(WaitBlock* block);
  void RemoveBlock(WaitBlock* block);
  void* address() { return &word_; }  // the kernel reads the low 32 bits there
  const char* what() const;
  static bool TryAny(PosixConditionBase* const* objects, const WaitBlock* blocks, size_t count,
                     size_t* index);
  static bool TryAll(PosixConditionBase* const* objects, const WaitBlock* blocks, size_t count);

  std::atomic<uint64_t> word_;
  std::atomic<uintptr_t> owner_{0};  // mutants: CurrentThreadToken() of the owner, 0 while free
  std::mutex blocks_mutex_;          // blocks_, and bit 48 changes
  WaitBlock* blocks_ = nullptr;
  const Kind kind_;
};

bool PosixConditionBase::Satisfiable(uint64_t state, uint32_t entry) const {
  const uint32_t value = ValueOf(state);
  switch (kind_) {
    case Kind::kManualEvent:
      return (value & 1) || (value >> 1) != (entry >> 1);
    case Kind::kAutoEvent:
    case Kind::kThread:
      return (value & 1) != 0;
    case Kind::kSemaphore:
      return value != 0;
    case Kind::kMutant:
      return value == 0 || owner_.load(std::memory_order_relaxed) == CurrentThreadToken();
  }
  return false;
}

uint32_t PosixConditionBase::Consumed(uint32_t value) const {
  switch (kind_) {
    case Kind::kAutoEvent:
      return value & ~1u;
    case Kind::kSemaphore:
      return value - 1;
    case Kind::kMutant:
      return value + 1;
    default:
      return value;
  }
}

bool PosixConditionBase::TryConsume(uint32_t entry) {
  uint64_t state = word_.load(std::memory_order_acquire);
  while (Satisfiable(state, entry)) {
    if (kind_ == Kind::kManualEvent || kind_ == Kind::kThread) {
      return true;  // nothing to take
    }
    if (word_.compare_exchange_weak(state, WithValue(state, Consumed(ValueOf(state))),
                                     std::memory_order_acq_rel, std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void PosixConditionBase::Acquired() {
  if (kind_ == Kind::kMutant) {
    owner_.store(CurrentThreadToken(), std::memory_order_release);
  }
  OnAcquired();
}

// Gives back what TryConsume took (WaitAll found another object taken in the meantime).
void PosixConditionBase::Unconsume() {
  switch (kind_) {
    case Kind::kAutoEvent:
      SetSignaled();
      break;
    case Kind::kSemaphore:
      AddCount(1, UINT32_MAX, nullptr);
      break;
    case Kind::kMutant: {
      const uint64_t old_state = word_.fetch_sub(1, std::memory_order_acq_rel);
      if (ValueOf(old_state) == 1) {
        WakeAfter(old_state, 1);
      }
    } break;
    default:
      break;
  }
}

void PosixConditionBase::WakeAfter(uint64_t old_state, int32_t single_count) {
  if ((old_state & kWaiterMask) != 0) {
    WakeAddress(address(), single_count);
  }
  if ((old_state & kMultiFlag) != 0) {
    std::lock_guard<std::mutex> lock(blocks_mutex_);
    for (WaitBlock* block = blocks_; block; block = block->next) {
      block->word->fetch_add(1, std::memory_order_acq_rel);
      WakeAddress(block->word, 1);
    }
  }
}

void PosixConditionBase::SetSignaled() {
  uint64_t state = word_.load(std::memory_order_relaxed);
  uint64_t next;
  do {
    const uint32_t value = ValueOf(state);
    if (value & 1) {
      return;  // already signaled: setting it again releases nobody new
    }
    next = WithValue(state, kind_ == Kind::kManualEvent ? (value + 2) | 1 : value | 1);
  } while (!word_.compare_exchange_weak(state, next, std::memory_order_acq_rel, std::memory_order_relaxed));
  WakeAfter(state, kind_ == Kind::kAutoEvent ? 1 : -1);
}

// PulseEvent: releases the threads waiting now and leaves the event non-signaled. Manual-reset: all of them (a
// new generation). Auto-reset: one, which takes the signal (as with SetEvent). With nobody waiting the pulse only
// resets the event, as on Windows.
void PosixConditionBase::PulseSignaled() {
  uint64_t state = word_.load(std::memory_order_relaxed);
  uint64_t next;
  do {
    const uint32_t value = ValueOf(state);
    uint32_t next_value;
    if ((state & kAnyWaiter) == 0) {
      next_value = value & ~1u;
    } else if (kind_ == Kind::kManualEvent) {
      next_value = (value & ~1u) + 2;
    } else {
      next_value = value | 1;
    }
    next = WithValue(state, next_value);
  } while (!word_.compare_exchange_weak(state, next, std::memory_order_acq_rel, std::memory_order_relaxed));
  if ((state & kAnyWaiter) != 0) {
    WakeAfter(state, kind_ == Kind::kManualEvent ? -1 : 1);
  }
}

bool PosixConditionBase::AddCount(uint32_t count, uint32_t maximum, uint32_t* previous) {
  uint64_t state = word_.load(std::memory_order_relaxed);
  do {
    if (count > maximum - ValueOf(state)) {
      return false;
    }
  } while (!word_.compare_exchange_weak(state, WithValue(state, ValueOf(state) + count),
                                         std::memory_order_acq_rel, std::memory_order_relaxed));
  if (previous) {
    *previous = ValueOf(state);
  }
  WakeAfter(state, int32_t(std::min<uint32_t>(count, INT32_MAX)));
  return true;
}

bool PosixConditionBase::ReleaseOwnership() {
  if (owner_.load(std::memory_order_relaxed) != CurrentThreadToken()) {
    return false;
  }
  // While the count is not zero only the owner changes it, so plain decrements are enough.
  const uint32_t count = ValueOf(word_.load(std::memory_order_acquire));
  if (count == 0) {
    return false;
  }
  if (count > 1) {
    word_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
  }
  owner_.store(0, std::memory_order_release);
  WakeAfter(word_.fetch_sub(1, std::memory_order_acq_rel), 1);
  return true;
}

void PosixConditionBase::AddBlock(WaitBlock* block) {
  std::lock_guard<std::mutex> lock(blocks_mutex_);
  block->prev = nullptr;
  block->next = blocks_;
  if (blocks_) {
    blocks_->prev = block;
  }
  blocks_ = block;
  block->entry = ValueOf(word_.fetch_or(kMultiFlag, std::memory_order_acq_rel));
}

void PosixConditionBase::RemoveBlock(WaitBlock* block) {
  std::lock_guard<std::mutex> lock(blocks_mutex_);
  if (block->prev) {
    block->prev->next = block->next;
  } else {
    blocks_ = block->next;
  }
  if (block->next) {
    block->next->prev = block->prev;
  }
  if (!blocks_) {
    word_.fetch_and(~kMultiFlag, std::memory_order_acq_rel);
  }
}

const char* PosixConditionBase::what() const {
  switch (kind_) {
    case Kind::kManualEvent:
      return "manual-reset event";
    case Kind::kAutoEvent:
      return "auto-reset event";
    case Kind::kSemaphore:
      return "semaphore";
    case Kind::kMutant:
      return "mutant";
    case Kind::kThread:
      return "thread exit";
  }
  return "object";
}

WaitResult PosixConditionBase::Wait(std::chrono::milliseconds timeout) {
  // Take the object, or register as a waiter: one compare-and-swap decides.
  uint64_t state = word_.load(std::memory_order_acquire);
  for (;;) {
    if (Satisfiable(state, ValueOf(state))) {
      if (word_.compare_exchange_weak(state, WithValue(state, Consumed(ValueOf(state))),
                                       std::memory_order_acq_rel, std::memory_order_acquire)) {
        Acquired();
        return WaitResult::kSuccess;
      }
    } else if (timeout.count() <= 0) {
      return WaitResult::kTimeout;
    } else if ((state & kWaiterMask) == kWaiterMask) {
      // 65,535 threads already sleep here (never in practice): wait on the thread's own word instead.
      PosixConditionBase* self = this;
      return WaitMultiple(&self, 1, false, false, timeout).first;
    } else if (word_.compare_exchange_weak(state, state + kWaiterOne, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      state += kWaiterOne;
      break;
    }
  }

  const uint32_t entry = ValueOf(state);
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = DeadlineAfter(start, timeout);
  bool reported = false;
  for (;;) {
    const int64_t sleep_ns = NextSleepNs(deadline);
    const AddressWait woke =
        sleep_ns > 0 ? WaitOnAddress(address(), ValueOf(state), sleep_ns) : AddressWait::kTimeout;
    const bool expired = std::chrono::steady_clock::now() >= deadline;
    // Leave with the object or on timeout, unregistering in the same compare-and-swap.
    state = word_.load(std::memory_order_acquire);
    for (;;) {
      if (Satisfiable(state, entry)) {
        if (word_.compare_exchange_weak(state, WithValue(state, Consumed(ValueOf(state))) - kWaiterOne,
                                         std::memory_order_acq_rel, std::memory_order_acquire)) {
          if (woke == AddressWait::kTimeout && !expired) {
            NoteLostWakeup(what(), start);
          }
          Acquired();
          return WaitResult::kSuccess;
        }
      } else if (expired) {
        if (word_.compare_exchange_weak(state, state - kWaiterOne, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
          return WaitResult::kTimeout;
        }
      } else {
        break;
      }
    }
    if (woke == AddressWait::kTimeout) {
      NoteLongWait(what(), start, &reported);
    }
  }
}

bool PosixConditionBase::TryAny(PosixConditionBase* const* objects, const WaitBlock* blocks, size_t count,
                                size_t* index) {
  for (size_t i = 0; i < count; ++i) {
    PosixConditionBase* object = objects[i];
    const uint32_t entry = blocks ? blocks[i].entry : ValueOf(object->word_.load(std::memory_order_acquire));
    if (object->TryConsume(entry)) {
      object->Acquired();
      *index = i;
      return true;
    }
  }
  return false;
}

bool PosixConditionBase::TryAll(PosixConditionBase* const* objects, const WaitBlock* blocks, size_t count) {
  std::lock_guard<std::mutex> lock(WaitAllMutex());
  auto entry = [&](size_t i) {
    return blocks ? blocks[i].entry : ValueOf(objects[i]->word_.load(std::memory_order_acquire));
  };
  for (size_t i = 0; i < count; ++i) {
    if (!objects[i]->Satisfiable(objects[i]->word_.load(std::memory_order_acquire), entry(i))) {
      return false;
    }
  }
  // All of them at once: take each, and give back what was taken if one went in the meantime.
  size_t taken = 0;
  while (taken < count && objects[taken]->TryConsume(entry(taken))) {
    ++taken;
  }
  if (taken < count) {
    for (size_t i = 0; i < taken; ++i) {
      objects[i]->Unconsume();
    }
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    objects[i]->Acquired();
  }
  return true;
}

std::pair<WaitResult, size_t> PosixConditionBase::WaitMultiple(PosixConditionBase* const* objects,
                                                               size_t count, bool wait_all,
                                                               bool alertable,
                                                               std::chrono::milliseconds timeout) {
  size_t index = 0;
  if (wait_all ? TryAll(objects, nullptr, count) : TryAny(objects, nullptr, count, &index)) {
    return {WaitResult::kSuccess, index};
  }
  if (alertable && DispatchCurrentThreadUserCallback()) {
    return {WaitResult::kUserCallback, 0};
  }
  if (timeout.count() <= 0) {
    return {WaitResult::kTimeout, 0};
  }

  std::atomic<uint32_t>* word = CurrentThreadWakeWord();
  constexpr size_t kInlineBlocks = 64;  // the Xbox's MAXIMUM_WAIT_OBJECTS
  WaitBlock inline_blocks[kInlineBlocks];
  std::unique_ptr<WaitBlock[]> heap_blocks;
  WaitBlock* blocks = inline_blocks;
  if (count > kInlineBlocks) {
    heap_blocks.reset(new WaitBlock[count]);
    blocks = heap_blocks.get();
  }
  for (size_t i = 0; i < count; ++i) {
    blocks[i].word = word;
    objects[i]->AddBlock(&blocks[i]);
  }

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = DeadlineAfter(start, timeout);
  std::pair<WaitResult, size_t> result{WaitResult::kTimeout, 0};
  bool reported = false;
  bool woke_by_check = false;
  for (;;) {
    // Read the word before looking at the objects: a signal after this read changes it, so the sleep below
    // returns at once.
    const uint32_t seen = word->load(std::memory_order_acquire);
    if (wait_all ? TryAll(objects, blocks, count) : TryAny(objects, blocks, count, &index)) {
      if (woke_by_check) {
        NoteLostWakeup(objects[index]->what(), start);
      }
      result = {WaitResult::kSuccess, index};
      break;
    }
    if (alertable && DispatchCurrentThreadUserCallback()) {
      result = {WaitResult::kUserCallback, 0};
      break;
    }
    const int64_t sleep_ns = NextSleepNs(deadline);
    if (sleep_ns == 0) {
      break;
    }
    woke_by_check = WaitOnAddress(word, seen, sleep_ns) == AddressWait::kTimeout &&
                    std::chrono::steady_clock::now() < deadline;
    if (woke_by_check) {
      NoteLongWait(objects[0]->what(), start, &reported);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    objects[i]->RemoveBlock(&blocks[i]);
  }
  return result;
}

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : PosixConditionBase(manual_reset ? Kind::kManualEvent : Kind::kAutoEvent, initial_state ? 1 : 0) {}
  virtual ~PosixCondition() = default;

  bool Signal() override {
    SetSignaled();
    return true;
  }
  void Reset() { ResetSignaled(); }
  void Pulse() { PulseSignaled(); }
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : PosixConditionBase(Kind::kSemaphore, initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    uint32_t previous = 0;
    if (!AddCount(release_count, maximum_count_, &previous)) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = static_cast<int>(previous);
    }
    return true;
  }

 private:
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : PosixConditionBase(Kind::kMutant, initial_owner ? 1 : 0) {}

  bool Signal() override { return Release(); }
  bool Release() { return ReleaseOwnership(); }
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : PosixConditionBase(manual_reset ? Kind::kManualEvent : Kind::kAutoEvent, 0), callback_(nullptr) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    SetSignaled();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(timer_mutex_);

    callback_ = std::move(opt_callback);
    ResetSignaled();
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(timer_mutex_);

    callback_ = std::move(opt_callback);
    ResetSignaled();
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
      std::lock_guard<std::mutex> lock(timer->timer_mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;  // Protected by timer_mutex_
  std::mutex timer_mutex_;
};

#else  // !REX_FAST_WAITS

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
    bool executed;
#if defined(__SWITCH__)
    const auto deadline = timeout == std::chrono::milliseconds::max()
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + timeout;
    // A plain blocking lock. The earlier try_lock + 100 us sleep loop (so that the timeout also
    // covered taking the lock) was most of the samples of BO2's job worker threads in a console profile, and
    // every sleep delayed the wake-up. This mutex is only held for a few
    // instructions (Signal, Reset, Pulse, the predicate), and Horizon's kernel mutex lends the holder the
    // waiter's priority, so a blocked waiter cannot starve it.
    std::unique_lock<std::mutex> lock(mutex_);
#elif REX_PLATFORM_LINUX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
#else
    std::unique_lock<std::mutex> lock(mutex_);
#endif
    // A PulseEvent releases the threads waiting at that moment (pulses() changes) without leaving
    // the event signaled; see PosixCondition<Event>::Pulse.
    const uint64_t initial_pulse = pulses();
    auto predicate = [this, initial_pulse] { return this->signaled() || this->pulses() != initial_pulse; };
    if (predicate()) {
      executed = true;
    } else {
      ++waiters_;
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
#if defined(__SWITCH__)
        // Do not start another full timeout after acquiring the lock.
        executed = cond_.wait_until(lock, deadline, predicate);
#else
        executed = cond_.wait_for(lock, timeout, predicate);
#endif
      }
      --waiters_;
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    } else {
      return WaitResult::kTimeout;
    }
  }

  static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    // Reuse the allocation between polls; clear() releases the locks, not the memory.
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(handles.size());
    while (true) {
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;
      bool all_locked = true;

      for (size_t i = 0; i < handles.size(); ++i) {
#if REX_PLATFORM_LINUX
        auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#else
        locks.emplace_back(handles[i]->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all_locked = false;
          break;
        }
#endif
      }

      if (!all_locked) {
        locks.clear();
        // The timeout also expires when an object cannot be inspected.
        // The earlier continue skipped the only timeout check.
        const auto now = std::chrono::steady_clock::now();
        if (now >= end_time) {
          return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
        }
#if defined(__SWITCH__)
        // On Horizon yielding is useless against a lower-priority thread:
        // svcSleepThread(0) only yields to threads of the same priority. If the
        // lock holder has lower priority and is on this core, it would never get
        // to release the lock. A real sleep is needed.
        const auto pause = timeout == std::chrono::milliseconds::max()
                               ? std::chrono::microseconds(100)
                               : std::min(std::chrono::ceil<std::chrono::microseconds>(end_time - now),
                                          std::chrono::microseconds(100));
        std::this_thread::sleep_for(pause);
#else
        std::this_thread::yield();
#endif
        continue;
      }

      if (wait_all) {
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
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
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      locks.clear();

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

 protected:
  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  // How many PulseEvents so far (events only), and how many threads are inside Wait's condition
  // variable wait. Both under mutex_.
  inline virtual uint64_t pulses() const { return 0; }
  uint32_t waiters_ = 0;
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

  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = true;
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
      ++pulse_count_;
      signal_ = false;
      cond_.notify_all();
    } else if (waiters_ > 0) {
      signal_ = true;
      cond_.notify_one();
    } else {
      signal_ = false;
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  inline uint64_t pulses() const override { return pulse_count_; }
  bool signal_;
  const bool manual_reset_;
  uint64_t pulse_count_ = 0;
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
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

#endif  // REX_FAST_WAITS

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
#elif defined(__SWITCH__)
  // pthread_t is a pointer in newlib, not an integer: it has to be reinterpreted.
  uint32_t system_id() const {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread_));
  }
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

#if defined(__SWITCH__)
    // Horizon does not deliver signals, so there is no way to interrupt another
    // thread to make it run the call. The callback is queued and runs when the
    // target thread passes an alertable point (DispatchCurrentThreadUserCallback),
    // which is where the SDK already drains them.
    //
    // A real difference in behavior: on the PC the callback arrives right away;
    // here it arrives when the thread gets to it. That is enough for guest APCs,
    // which is what the SDK uses this for.
    int result = 0;
#elif defined(__APPLE__)
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
#if REX_FAST_WAITS
    // An alertable wait sleeps on this word (bumped after the callback is queued above), so the
    // callback ends it now instead of at the next 1 ms poll.
    wake_word_.fetch_add(1, std::memory_order_acq_rel);
    WakeAddress(&wake_word_, 1);
#endif
  }

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
#if defined(__SWITCH__)
      // Counterpart of RexSwitchPauseThread in Suspend(). The semaphore below
      // wakes a thread that suspended itself from WaitSuspended(); this call
      // resumes a thread that was paused from outside. Both are needed,
      // because they are two different suspension paths.
      if (!pthread_equal(thread_, pthread_self())) {
        RexSwitchResumeThread(thread_);
      }
#endif
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
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

#if defined(__SWITCH__)
    // Careful: pthread_kill does not work here.
    //
    // On Horizon newlib accepts the call and returns 0, but the signal is never
    // delivered. The result would be the worst possible: the state would say
    // kSuspended, the function would return true, and the thread would keep
    // running happily. A bug that compiles, gives no warning and shows up later
    // as random corruption.
    //
    // svcSetThreadActivity is the system's real mechanism.
    return RexSwitchPauseThread(thread_) == 0;
#else
    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
#endif
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

#if REX_FAST_WAITS
    exit_code_ = exit_code;
    SetSignaled();
#else
    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
    }
#endif
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

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(suspend_sem_ptr());
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }

#if REX_FAST_WAITS
  // Multi-object and alertable waits of this thread sleep on this word; signalers and QueueUserCallback bump it.
  std::atomic<uint32_t>* wake_word() { return &wake_word_; }
#endif

 private:
  static void* ThreadStartRoutine(void* parameter);
#if REX_FAST_WAITS
  // Every successful wait used to run this; join only once (a second pthread_join is undefined).
  void OnAcquired() override {
    if (joined_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
#else
  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
#endif
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
#if REX_FAST_WAITS
  std::atomic<uint32_t> wake_word_{0};
  std::atomic<bool> joined_{false};
#endif
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

#if REX_FAST_WAITS
// For the stress test (app/src/bo2zm_test_waits.cpp): how many threads wait on handle right now, so it can pulse
// only once they are all inside the wait.
uint32_t FastWaitWaiters(WaitHandle* handle) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(handle);
  return posix_wait_handle ? posix_wait_handle->condition().waiters() : 0;
}

namespace {

std::atomic<uint32_t>* CurrentThreadWakeWord() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_->wake_word();
}

}  // namespace

// Sleeps on the thread's wake word, so a queued user callback (APC) ends the sleep at once. The
// original polled the callback queue every millisecond.
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  std::atomic<uint32_t>* word = CurrentThreadWakeWord();
  const auto deadline = std::chrono::steady_clock::now() + duration;
  for (;;) {
    const uint32_t seen = word->load(std::memory_order_acquire);
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    const int64_t sleep_ns = NextSleepNs(deadline);
    if (sleep_ns == 0) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    WaitOnAddress(word, seen, sleep_ns);
  }
}
#endif  // REX_FAST_WAITS

namespace {

#if !REX_FAST_WAITS
constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);
#endif

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

#if !REX_FAST_WAITS
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
#endif  // !REX_FAST_WAITS

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

#if REX_FAST_WAITS
WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  PosixConditionBase* condition = &posix_wait_handle->condition();
  if (!is_alertable) {
    return condition->Wait(timeout);
  }
  ScopedAlertableState alertable_state_guard(true);
  return PosixConditionBase::WaitMultiple(&condition, 1, false, true, timeout).first;
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }
  PosixConditionBase* condition = &posix_wait_handle_to_wait_on->condition();
  if (!is_alertable) {
    return condition->Wait(timeout);
  }
  ScopedAlertableState alertable_state_guard(true);
  return PosixConditionBase::WaitMultiple(&condition, 1, false, true, timeout).first;
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  if (wait_handle_count == 0) {
    return std::make_pair(WaitResult::kFailed, 0);
  }
  // No allocation for the usual counts (each malloc takes newlib's global heap lock).
  constexpr size_t kInlineConditions = 64;
  PosixConditionBase* inline_conditions[kInlineConditions];
  std::unique_ptr<PosixConditionBase*[]> heap_conditions;
  PosixConditionBase** conditions = inline_conditions;
  if (wait_handle_count > kInlineConditions) {
    heap_conditions.reset(new PosixConditionBase*[wait_handle_count]);
    conditions = heap_conditions.get();
  }
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions[i] = &handle->condition();
  }
  if (wait_handle_count == 1 && !is_alertable) {
    return std::make_pair(conditions[0]->Wait(timeout), size_t(0));
  }
  ScopedAlertableState alertable_state_guard(is_alertable);
  return PosixConditionBase::WaitMultiple(conditions, wait_handle_count, wait_all, is_alertable, timeout);
}
#else  // !REX_FAST_WAITS
WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (!is_alertable) {
    return posix_wait_handle->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);

  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
    // Even with a zero timeout the signaled object has to be checked once.
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }

  if (!is_alertable) {
    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
  }
}
#endif  // REX_FAST_WAITS

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
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    assert_always();
  }
#endif
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
#if defined(__SWITCH__)
  // The pthread_t -> Horizon handle registry (threading_switch.h), before the started state is published: every
  // priority or affinity change made from another thread looks the handle up there. Without this registration every
  // such change fails with ESRCH ("set_priority: pthread_setschedparam failed (3)").
  RexSwitchRegisterCurrentThread();
#elif defined(__APPLE__)
  // Guest threads (game, render, audio) at the user-interactive QoS class, as the main thread already is: the
  // default class lets macOS move them to efficiency cores and delay their wake-ups. Only a thread itself can set
  // its class once it runs.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
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

#if defined(__SWITCH__)
  RexSwitchUnregisterCurrentThread();
#endif
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

#if REX_FAST_WAITS
  thread->handle_.exit_code_ = 0;
  thread->handle_.SetSignaled();
#else
  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.cond_.notify_all();
  }
#endif

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
[[gnu::noinline]] static void suspend_current_thread_from_signal() {
  PosixThread* thread = current_thread_;
  if (thread) {
    thread->WaitSuspended();
  }
}

static void signal_handler(int signal, siginfo_t* /*info*/, void* /*context*/) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      suspend_current_thread_from_signal();
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
