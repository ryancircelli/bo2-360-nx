/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/thread_spin.h>

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_DOUBLE(guest_time_scale, 1.0, "Clock",
                      "Test fast-forward: run guest time this many times faster than real time (1.0 = off, "
                      "applied at start-up, 0.1 steps, clamped to 0.1..16). Games drive their own physics, audio "
                      "and AI from guest time, so values above ~2 can change behaviour; for test routes only.");

REXCVAR_DEFINE_DOUBLE(clock_frame_lock_hz, 0.0, "Clock",
                      "Deterministic benchmark: guest time advances 1/hz per presented frame instead of with wall "
                      "time (0 = off). Within a frame it follows wall time but stops at the frame's step, so a slow "
                      "or starved frame still covers exactly one step and the game does the same work per frame.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(clock_frame_lock_stall_ms, 1000, "Clock",
                     "With clock_frame_lock_hz: after this many wall ms without a frame (loading, boot) guest time "
                     "runs with wall time again until the next frame, so nothing waits forever on a stopped clock.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(clock_read_spin_backoff, false, "Clock",
                    "Bench knob (off = old behaviour): a thread that reads the guest clock back to back (under 1 us "
                    "apart, 256 reads in a row: a frame-limiter spin) goes through the guest poll backoff, so it "
                    "sleeps instead of burning its core");
REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// The guest clock without a lock.
//
// Why: every guest mftb (REX_QUERY_TIMEBASE in the generated code, 101 sites in BO2) reads the clock through
// UpdateGuestClock, which took tick_mutex_ on every read: in a console profile, 5.4 % of JQ Worker 1's samples and
// 3.2 % of the Backend's (pthread_mutex_lock <- UpdateGuestClock <- sub_8284AFB8). It also added up each read's
// delta rounded down, so a read lost up to one guest tick and the guest clock ran slower the more often it was
// read: at the Switch's 125/48 (19.2 MHz host, 50 MHz guest) by up to a tick per read; on Linux (ns host clock,
// 1/20) four threads reading in a loop measured -22.5 % (the guest_clock test in games/bo2/src/bo2zm_test_waits.cpp).
//
// How: guest ticks are a function of the host tick count, from an anchor (host and guest count at the last ratio
// change, and the ratio) published through one pointer. A read is one atomic load, the counter read and integer
// arithmetic, exact. Only RecomputeGuestTickScalar (start-up, time scalar changes) makes a new anchor, under
// tick_mutex_, continuing from the old one's value at that moment. Old anchors are never freed (one per ratio
// change), so a reader still holding one is safe.
namespace {

struct GuestClockAnchor {
  uint64_t host;
  uint64_t guest;
  uint64_t numerator;    // guest ticks per host tick: numerator / denominator
  uint64_t denominator;
};
// Guest ticks start at 0 when the process starts, at the initial 1:1 ratio (as last_guest_tick_count_ did).
const GuestClockAnchor initial_guest_clock_anchor_{Clock::QueryHostTickCount(), 0, 1, 1};
std::atomic<const GuestClockAnchor*> guest_clock_anchor_{nullptr};  // nullptr: the initial one

const GuestClockAnchor& CurrentGuestClockAnchor() {
  const GuestClockAnchor* anchor = guest_clock_anchor_.load(std::memory_order_acquire);
  return anchor ? *anchor : initial_guest_clock_anchor_;
}

uint64_t GuestTicksAt(const GuestClockAnchor& anchor, uint64_t host_tick_count) {
  if (host_tick_count <= anchor.host) {
    return anchor.guest;
  }
  // delta * numerator / denominator without overflowing: whole denominators first, then the remainder.
  const uint64_t delta = host_tick_count - anchor.host;
  const uint64_t whole = delta / anchor.denominator;
  const uint64_t rest = delta % anchor.denominator;
  uint64_t rest_ticks;
  if (anchor.numerator <= std::numeric_limits<uint64_t>::max() / anchor.denominator) {
    rest_ticks = rest * anchor.numerator / anchor.denominator;
  } else {
    rest_ticks = static_cast<uint64_t>(static_cast<long double>(rest) * anchor.numerator / anchor.denominator);
  }
  return anchor.guest + whole * anchor.numerator + rest_ticks;
}

}  // namespace

// Frame lock (clock_frame_lock_hz): guest = base + min(elapsed since the last frame, step), or after a stall
// base + step + (elapsed - stall). OnGuestFrame moves base to max(base + step, guest now), so the clock never goes
// back. One writer (the swap thread); readers use a seqlock.
namespace {
std::atomic<uint32_t> frame_lock_seq_{0};
uint64_t frame_lock_host_ = 0;   // host tick count of the last frame
uint64_t frame_lock_base_ = 0;   // guest ticks at the last frame
std::atomic<uint64_t> frame_lock_frames_{0}, frame_lock_stalls_{0};
uint64_t frame_lock_step_ = 0;   // guest ticks per frame, 0 = off
uint64_t frame_lock_stall_ = 0;  // guest ticks

void InitFrameLock() {
  static std::once_flag once;
  std::call_once(once, [] {
    const double hz = REXCVAR_GET(clock_frame_lock_hz);
    if (hz > 0.0) {
      frame_lock_step_ = std::max<uint64_t>(1, uint64_t(double(guest_tick_frequency_) / hz));
      frame_lock_stall_ = uint64_t(double(guest_tick_frequency_) * std::max(1, REXCVAR_GET(clock_frame_lock_stall_ms)) / 1000.0);
      frame_lock_host_ = Clock::QueryHostTickCount();
    }
  });
}

uint64_t FrameLocked(uint64_t elapsed) {  // elapsed guest ticks since the last frame -> ticks above its base
  if (elapsed <= frame_lock_stall_) {
    return std::min(elapsed, frame_lock_step_);
  }
  return frame_lock_step_ + (elapsed - frame_lock_stall_);
}
}  // namespace

// Serializes ratio changes (guest_tick_ratio_ and the anchor).
std::mutex tick_mutex_;

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<std::mutex> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
  // The guest clock goes on from where it is now, at the new ratio.
  const uint64_t host_tick_count = Clock::QueryHostTickCount();
  const GuestClockAnchor& anchor = CurrentGuestClockAnchor();
  guest_clock_anchor_.store(
      new GuestClockAnchor{host_tick_count, GuestTicksAt(anchor, host_tick_count), frac.first, frac.second},
      std::memory_order_release);
}

// The guest tick count now (lock-free, see GuestClockAnchor).
uint64_t UpdateGuestClock() {
  // The anchor before the counter, so the counter is never older than the anchor.
  const GuestClockAnchor& anchor = CurrentGuestClockAnchor();
  const uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to update, calculate on the fly
    return GuestTicksAt(GuestClockAnchor{0, 0, anchor.numerator, anchor.denominator}, host_tick_count);
  }
  InitFrameLock();
  if (frame_lock_step_) [[unlikely]] {
    for (;;) {
      const uint32_t seq = frame_lock_seq_.load(std::memory_order_acquire);
      if (seq & 1) continue;
      const uint64_t host = frame_lock_host_, base = frame_lock_base_;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (frame_lock_seq_.load(std::memory_order_relaxed) != seq) continue;
      const uint64_t now = Clock::QueryHostTickCount();
      const uint64_t elapsed = now > host ? GuestTicksAt(GuestClockAnchor{0, 0, anchor.numerator, anchor.denominator}, now - host) : 0;
      return base + FrameLocked(elapsed);
    }
  }

  return GuestTicksAt(anchor, host_tick_count);
}

void Clock::OnGuestFrame() {
  InitFrameLock();
  if (!frame_lock_step_) {
    return;
  }
  const uint64_t now_guest = UpdateGuestClock();
  const uint64_t elapsed = now_guest - frame_lock_base_;
  if (elapsed > frame_lock_step_) {
    frame_lock_stalls_.fetch_add(1, std::memory_order_relaxed);
  }
  frame_lock_seq_.fetch_add(1, std::memory_order_acq_rel);  // odd: readers wait
  frame_lock_base_ += std::max(frame_lock_step_, elapsed);
  frame_lock_host_ = Clock::QueryHostTickCount();
  frame_lock_seq_.fetch_add(1, std::memory_order_release);
  frame_lock_frames_.fetch_add(1, std::memory_order_relaxed);
}

bool Clock::frame_locked() {
  InitFrameLock();
  return frame_lock_step_ != 0;
}
uint64_t Clock::frame_lock_frames() { return frame_lock_frames_.load(std::memory_order_relaxed); }
uint64_t Clock::frame_lock_base() { return frame_lock_base_; }
void Clock::RewindFrameLock(uint64_t base) {
  frame_lock_seq_.fetch_add(1, std::memory_order_acq_rel);
  frame_lock_base_ = base;
  frame_lock_host_ = Clock::QueryHostTickCount();
  frame_lock_seq_.fetch_add(1, std::memory_order_release);
}
uint64_t Clock::frame_lock_stalls() { return frame_lock_stalls_.load(std::memory_order_relaxed); }

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  if (REXCVAR_GET(clock_read_spin_backoff)) [[unlikely]] {
    // shortcut: the first 256 reads of a spin run are not counted as spin; fine for the bench, upgrade if it matters.
    static thread_local uint64_t last_ns = 0;
    static thread_local uint32_t streak = 0;
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
    streak = now - last_ns < 1000 ? streak + 1 : 0;
    last_ns = now;
    if (streak >= 256) rex::thread::GuestPoll();
  }
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

void Clock::RestoreGuestClock(uint64_t guest_tick_count, uint64_t system_time_base) {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  const GuestClockAnchor& anchor = CurrentGuestClockAnchor();
  guest_clock_anchor_.store(
      new GuestClockAnchor{Clock::QueryHostTickCount(), guest_tick_count, anchor.numerator, anchor.denominator},
      std::memory_order_release);
  guest_system_time_base_ = system_time_base;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono

// `runq live exec S cmd clock_status` (twice, some wall seconds apart): guest vs host seconds, to tell a slow guest
// clock from a title that is waiting (ME2's "slow" opening cinematic was a dialogue wheel waiting for a choice).
REXCVAR_DEFINE_COMMAND(
    clock_status,
    []() {
      REXLOG_INFO("[clock] guest_s={:.3f} host_s={:.3f} scalar={} guest_hz={} frame_lock={}",
                  double(rex::chrono::Clock::QueryGuestTickCount()) / double(rex::chrono::Clock::guest_tick_frequency()),
                  double(rex::chrono::Clock::QueryHostTickCount()) / double(rex::chrono::Clock::QueryHostTickFrequency()),
                  rex::chrono::Clock::guest_time_scalar(), rex::chrono::Clock::guest_tick_frequency(),
                  rex::chrono::Clock::frame_locked());
    },
    "Clock", "Log guest and host clock seconds (call twice to compare rates)");
