#include <rex/input/input_record.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <fmt/format.h>

namespace rex::input {

void InputRing::Record(const PadSample& s) {
  if (s.user >= 4) return;
  std::lock_guard lock(mutex_);
  if (last_valid_[s.user] && last_[s.user].SameInputs(s)) return;
  last_[s.user] = s;
  last_valid_[s.user] = true;
  buf_[head_] = s;
  head_ = (head_ + 1) % kCapacity;
  count_ = std::min(count_ + 1, kCapacity);
}

std::vector<PadSample> InputRing::Window(uint32_t now_ms, uint32_t window_ms) const {
  std::lock_guard lock(mutex_);
  const uint32_t from = now_ms > window_ms ? now_ms - window_ms : 0;
  std::vector<PadSample> in_window;
  PadSample before[4];
  bool have_before[4] = {};
  for (size_t i = 0; i < count_; i++) {
    const PadSample& s = buf_[(head_ + kCapacity - count_ + i) % kCapacity];
    if (s.ms < from) {
      before[s.user] = s;
      have_before[s.user] = true;
    } else {
      in_window.push_back(s);
    }
  }
  std::vector<PadSample> out;
  for (uint32_t u = 0; u < 4; u++) {
    if (have_before[u]) {
      out.push_back(before[u]);
      out.back().ms = from;
    }
  }
  out.insert(out.end(), in_window.begin(), in_window.end());
  return out;
}

void InputRing::Clear() {
  std::lock_guard lock(mutex_);
  head_ = count_ = 0;
  std::fill(std::begin(last_valid_), std::end(last_valid_), false);
}

InputRing& GlobalInputRing() {
  static InputRing ring;
  return ring;
}

static std::atomic<void*> g_frame_ctx{nullptr};
static std::atomic<uint64_t (*)(void*)> g_frame_fn{nullptr};
void SetFrameSource(uint64_t (*fn)(void*), void* ctx) {
  g_frame_ctx.store(ctx, std::memory_order_relaxed);
  g_frame_fn.store(fn, std::memory_order_release);
}
uint64_t GuestFrame() {
  auto fn = g_frame_fn.load(std::memory_order_acquire);
  return fn ? fn(g_frame_ctx.load(std::memory_order_relaxed)) : 0;
}

namespace {
std::mutex g_clock_mutex;
std::function<double()> g_clock;
}  // namespace

void SetProcessClock(std::function<double()> clock) {
  std::lock_guard lock(g_clock_mutex);
  g_clock = std::move(clock);
}

uint32_t ProcessMs() {
  {
    std::lock_guard lock(g_clock_mutex);
    if (g_clock) return static_cast<uint32_t>(g_clock() * 1000.0);
  }
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

std::string SamplesToText(const std::vector<PadSample>& samples) {
  std::string out = "# glitch-input v1: ms frame user buttons(hex) lt rt lx ly rx ry\n";
  const uint32_t base = samples.empty() ? 0 : samples.front().ms;
  for (const PadSample& s : samples) {
    out += fmt::format("{} {} {} {:x} {} {} {} {} {} {}\n", s.ms - std::min(s.ms, base), s.frame, s.user,
                       s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry);
  }
  return out;
}

std::vector<PadSample> SamplesFromText(std::string_view text) {
  std::vector<PadSample> out;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    unsigned long long ms, frame, user, buttons, lt, rt;
    int lx, ly, rx, ry;
    if (std::sscanf(line.c_str(), "%llu %llu %llu %llx %llu %llu %d %d %d %d", &ms, &frame, &user, &buttons, &lt,
                    &rt, &lx, &ly, &rx, &ry) != 10 ||
        user >= 4 || lt > 255 || rt > 255) {
      continue;
    }
    PadSample s;
    s.ms = static_cast<uint32_t>(ms);
    s.frame = frame;
    s.user = static_cast<uint32_t>(user);
    s.buttons = static_cast<uint16_t>(buttons);
    s.lt = static_cast<uint8_t>(lt);
    s.rt = static_cast<uint8_t>(rt);
    s.lx = static_cast<int16_t>(lx);
    s.ly = static_cast<int16_t>(ly);
    s.rx = static_cast<int16_t>(rx);
    s.ry = static_cast<int16_t>(ry);
    out.push_back(s);
  }
  std::stable_sort(out.begin(), out.end(), [](const PadSample& a, const PadSample& b) { return a.ms < b.ms; });
  return out;
}

RecordedPadDriver::RecordedPadDriver(std::vector<PadSample> samples, double start_delay, Clock clock)
    : InputDriver(nullptr, 0), samples_(std::move(samples)), start_delay_(start_delay), clock_(std::move(clock)) {
  for (const PadSample& s : samples_) users_[s.user] = true;
  if (!clock_) {
    const auto start = std::chrono::steady_clock::now();
    clock_ = [start] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
  }
}

bool RecordedPadDriver::StateAt(uint32_t user, uint32_t ms, PadSample* out) const {
  bool found = false;
  for (const PadSample& s : samples_) {
    if (s.ms > ms) break;
    if (s.user == user) {
      *out = s;
      found = true;
    }
  }
  return found;
}

void RecordedPadDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  for (uint32_t u = 0; u < 4; u++) {
    if (!users_[u]) continue;
    DeviceInfo info;
    info.id = static_cast<DeviceId>(kBase + u);
    info.name = fmt::format("Recorded pad {}", u + 1);
    info.serial = fmt::format("recorded-pad-{}", u + 1);
    info.fixed_user = static_cast<int32_t>(u);
    info.transport = "replay";
    out.push_back(info);
  }
}

X_RESULT RecordedPadDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  const uint64_t u = static_cast<uint64_t>(id) - kBase;
  if (u >= 4 || !users_[u]) return X_ERROR_DEVICE_NOT_CONNECTED;
  const double t = clock_() - start_delay_;
  // Not connected until the replay starts, so a route's own virtual pad keeps the slot until then.
  if (t < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
  PadSample s;
  if (!StateAt(static_cast<uint32_t>(u), static_cast<uint32_t>(t * 1000.0), &s)) s = PadSample{};
  if (out_state) {
    *out_state = {};
    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = s.buttons;
    out_state->gamepad.left_trigger = s.lt;
    out_state->gamepad.right_trigger = s.rt;
    out_state->gamepad.thumb_lx = s.lx;
    out_state->gamepad.thumb_ly = s.ly;
    out_state->gamepad.thumb_rx = s.rx;
    out_state->gamepad.thumb_ry = s.ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT RecordedPadDriver::GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) {
  const uint64_t u = static_cast<uint64_t>(id) - kBase;
  if (u >= 4 || !users_[u]) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    *out_caps = {};
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

}  // namespace rex::input
