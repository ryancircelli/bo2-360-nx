#pragma once
/**
 * @file        rex/input/input_record.h
 * @brief       Always-on pad input recorder (a ring of state changes) and the replayer that feeds a dump back as
 *              a virtual pad. Used by the Guide's "Report a glitch".
 */

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>

namespace rex::input {

/// One pad state the guest saw (after the Guide gate), host-endian.
struct PadSample {
  uint32_t ms = 0;      // milliseconds since the process start
  uint64_t frame = 0;   // guest frame (GPU swap count) when it was read
  uint32_t user = 0;
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool SameInputs(const PadSample& o) const {
    return user == o.user && buttons == o.buttons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly &&
           rx == o.rx && ry == o.ry;
  }
};

/// Fixed-size ring of samples; a sample is stored only when a user's inputs change, so 16k entries cover
/// minutes of play. Record() is one mutex lock and a compare: cheap enough to run on every GetState.
class InputRing {
 public:
  static constexpr size_t kCapacity = 16384;
  InputRing() : buf_(kCapacity) {}
  void Record(const PadSample& s);
  /// Samples from the last `window_ms` before `now_ms`, oldest first. Each user's state in force at the start
  /// of the window is included first (stamped at the window start) so a replay begins from the right state.
  std::vector<PadSample> Window(uint32_t now_ms, uint32_t window_ms) const;
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::vector<PadSample> buf_;
  size_t head_ = 0;   // next write
  size_t count_ = 0;
  PadSample last_[4];
  bool last_valid_[4] = {};
};

/// The process-wide ring InputSystem::GetState feeds.
InputRing& GlobalInputRing();
/// Where the recorder reads the guest frame (GPU swap count) it stamps samples with; call once at start-up.
void SetFrameSource(uint64_t (*fn)(void*), void* ctx);
uint64_t GuestFrame();
/// Milliseconds since the process start (the clock of PadSample::ms).
uint32_t ProcessMs();
/// Makes ProcessMs() (and so the recorded timeline) follow another clock, in seconds; null restores the host clock.
void SetProcessClock(std::function<double()> clock);

/// "# glitch-input v1" + one line per sample: "ms frame user buttons(hex) lt rt lx ly rx ry". ms are rebased so
/// the first sample is at 0.
std::string SamplesToText(const std::vector<PadSample>& samples);
/// Reads that text back; bad lines are skipped.
std::vector<PadSample> SamplesFromText(std::string_view text);

/// Plays samples as pads on their users, starting `start_delay` seconds after the driver is made.
class RecordedPadDriver final : public InputDriver {
 public:
  using Clock = std::function<double()>;
  RecordedPadDriver(std::vector<PadSample> samples, double start_delay, Clock clock = nullptr);

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override { return X_ERROR_EMPTY; }

  static constexpr uint64_t kBase = 0x5245435000000000ull;  // "RECP"
  /// The state in force for `user` at `ms` into the replay (false when that user has no samples).
  bool StateAt(uint32_t user, uint32_t ms, PadSample* out) const;

 private:
  std::vector<PadSample> samples_;  // sorted by ms
  bool users_[4] = {};
  double start_delay_;
  Clock clock_;
  mutable uint32_t packet_ = 0;
};

}  // namespace rex::input
