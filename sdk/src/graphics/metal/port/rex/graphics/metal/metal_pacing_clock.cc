#include "rex/graphics/metal/metal_pacing_clock.h"

#include "rex/graphics/metal/metal_vsync_lock.h"
#include "rex/graphics/metal_runtime.h"
#include "rex/logging.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

namespace rex::graphics::metal::pacing {
namespace {
constexpr double kFitWindowS = 20.0;
constexpr double kFitMinSpanS = 10.0;
constexpr size_t kFitMinSamples = 240;
constexpr double kFitMaxRmsS = 0.5e-3;
constexpr double kFitMaxRejected = 0.10;
constexpr double kFitMaxRatioError = 0.025;
constexpr double kSlewPerTick = 2e-5;       // fraction of the interval per vblank tick
constexpr double kTimedOffsetS = 12.5e-3;   // desired present time after the guest vblank
constexpr double kTimedMinHz = 117.0;
constexpr int kWatchWindow = 120, kWatchBad = 60, kWatchNoReport = 180;

struct State {
  std::mutex mutex;
  std::atomic<double> nominal_hz{60.0};
  // Fit.
  std::deque<double> shown;  // presentedTime history (all drawables)
  double fitted = 0;         // accepted display period, 0 = none
  double fit_rms = 0;
  double next_fit_at = 0;
  // Slew state, worker thread only.
  double scale = 1.0;
  // Timed presents.
  std::atomic<bool> timed_off{false};
  int timed_sent = 0, timed_reported = 0;
  std::deque<uint8_t> timed_bad;
  // TSV log.
  FILE* log = nullptr;
  double last_flush = 0;
  bool log_init = false;
};
State& Get() {
  static State s;
  return s;
}
void OpenLog(State& s) {
  if (s.log_init) return;
  s.log_init = true;
  if (const char* path = MetalDebugEnv("PACING_LOG"); path && *path) {  // getenv-ok: once
    s.log = std::fopen(path, "a");
    if (s.log) REXLOG_INFO("[pacing] log {}", path);
  }
}
void LogRow(State& s, const char* kind, double t, double a, double b) {
  OpenLog(s);
  if (!s.log) return;
  std::fprintf(s.log, "%s\t%.6f\t%d\t%.1f\t%.1f\n", kind, t, int(Enabled()), a, b);
  if (t - s.last_flush > 2.0) {
    std::fflush(s.log);
    s.last_flush = t;
  }
}

// Fit period over the history. Caller holds the mutex.
void Refit(State& s, double now) {
  s.next_fit_at = now + 1.0;
  const double nominal = 1.0 / s.nominal_hz.load(std::memory_order_relaxed);
  while (!s.shown.empty() && now - s.shown.front() > kFitWindowS) s.shown.pop_front();
  if (s.shown.size() < kFitMinSamples || s.shown.back() - s.shown.front() < kFitMinSpanS) {
    s.fitted = 0;
    return;
  }
  // Sequential refresh indices (an index is the rounded number of periods since the previous sample, with the
  // running estimate of the period: no drift from a slightly wrong nominal).
  std::vector<double> idx(s.shown.size());
  double est = nominal;
  idx[0] = 0;
  for (size_t i = 1; i < s.shown.size(); ++i) {
    const double steps = std::max(1.0, std::round((s.shown[i] - s.shown[i - 1]) / est));
    idx[i] = idx[i - 1] + steps;
    if (i > 16 && i % 16 == 0) est = (s.shown[i] - s.shown[0]) / idx[i];
  }
  std::vector<char> keep(idx.size(), 1);
  double a = 0, b = nominal, rms = 0;
  size_t used = 0;
  for (int pass = 0; pass < 3; ++pass) {
    double sn = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      if (!keep[i]) continue;
      const double x = idx[i], y = s.shown[i] - s.shown[0];
      sn += 1, sx += x, sy += y, sxx += x * x, sxy += x * y;
    }
    const double den = sn * sxx - sx * sx;
    if (sn < 8 || den <= 0) {
      s.fitted = 0;
      return;
    }
    b = (sn * sxy - sx * sy) / den;
    a = (sy - b * sx) / sn;
    double ss = 0;
    used = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      if (!keep[i]) continue;
      const double r = s.shown[i] - s.shown[0] - (a + b * idx[i]);
      ss += r * r, ++used;
    }
    rms = std::sqrt(ss / double(used));
    if (pass == 2) break;
    const double cut = std::max(3.0 * rms, 0.2e-3);
    for (size_t i = 0; i < idx.size(); ++i) {
      const double r = s.shown[i] - s.shown[0] - (a + b * idx[i]);
      keep[i] = std::fabs(r) <= cut;
    }
  }
  const double rejected = 1.0 - double(used) / double(idx.size());
  const bool ok = rms <= kFitMaxRmsS && rejected <= kFitMaxRejected && std::fabs(b / nominal - 1.0) <= kFitMaxRatioError;
  const double old = s.fitted;
  s.fitted = ok ? b : 0;
  s.fit_rms = rms;
  if ((old == 0) != (s.fitted == 0) || (ok && std::fabs(old - b) > 20e-6)) {
    REXLOG_INFO("[pacing] fit {}: period {:.4f} ms ({:.3f} Hz), rms {:.3f} ms, rejected {:.1f}%, {} samples",
                ok ? "ok" : "rejected", b * 1e3, 1.0 / b, rms * 1e3, rejected * 100, idx.size());
  }
}
}  // namespace

bool Enabled() { return PacingClockRequested(); }

void SetNominalRefreshHz(double hz) {
  if (hz >= 20.0 && hz <= 500.0) Get().nominal_hz.store(hz, std::memory_order_relaxed);
}

void NoteTick(double tick_seconds, double lateness_seconds) {
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  LogRow(s, "T", tick_seconds, lateness_seconds * 1e6, 0);
}

double NextIntervalScale() {
  auto& s = Get();
  double target = 1.0;
  if (Enabled()) {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.fitted > 0) target = s.fitted * s.nominal_hz.load(std::memory_order_relaxed);
  }
  s.scale += std::clamp(target - s.scale, -kSlewPerTick, kSlewPerTick);
  return s.scale;
}

void OnPresented(double presented, double requested, bool new_frame) {
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  const double now = vsync_lock::NowSeconds();
  if (presented > 0) {
    s.shown.push_back(presented);
    if (now >= s.next_fit_at) Refit(s, now);
  }
  LogRow(s, "P", presented, new_frame ? 1.0 : 0.0, requested > 0 ? (presented - requested) * 1e6 : 0.0);
  if (requested > 0) {
    ++s.timed_reported;
    const double d = presented - requested;
    s.timed_bad.push_back(presented <= 0 || d < -2e-3 || d > 50e-3);
    if (int(s.timed_bad.size()) > kWatchWindow) s.timed_bad.pop_front();
    int bad = 0;
    for (uint8_t b : s.timed_bad) bad += b;
    if (!s.timed_off.load(std::memory_order_relaxed) && int(s.timed_bad.size()) >= kWatchWindow && bad >= kWatchBad) {
      s.timed_off.store(true, std::memory_order_relaxed);
      REXLOG_WARN("[pacing] timed presents off for the session: {} of {} bad", bad, s.timed_bad.size());
    }
  }
}

bool TimedPresentTarget(double vblank, double now, double* target) {
  auto& s = Get();
  if (!Enabled() || s.timed_off.load(std::memory_order_relaxed) || vblank <= 0) return false;
  if (s.nominal_hz.load(std::memory_order_relaxed) < kTimedMinHz) return false;
  const double t = vblank + kTimedOffsetS;
  if (t < now + 1e-3) return false;  // late: let it show when ready
  std::lock_guard<std::mutex> lock(s.mutex);
  // Requests that never report back (handler not called) mean the path is not honoured.
  if (++s.timed_sent - s.timed_reported >= kWatchNoReport) {
    s.timed_off.store(true, std::memory_order_relaxed);
    REXLOG_WARN("[pacing] timed presents off for the session: {} sent, {} reported", s.timed_sent, s.timed_reported);
    return false;
  }
  *target = t;
  return true;
}
}  // namespace rex::graphics::metal::pacing
