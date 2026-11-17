// SPDX-License-Identifier: 0BSD
// Pure decision logic for the BO2 cheat-sync side channel (spec G8-cheat-sync sections 3, 5.2, 6.2-6.3, 10.1):
// destination choice, retry timing, command-outcome memory, menu-requester table. No I/O, caller supplies time.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace bo2cs {

constexpr uint16_t kGuestSidePort = 1002;
constexpr int64_t kPeerLiveMs = 5000;

// Signed wrapping comparison of 32-bit sequences: true when a is newer than b.
inline bool SeqNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

struct DestinationInputs {
  std::optional<uint32_t> learned_host;   // source of the latest host message this match
  std::optional<uint32_t> peer_ip;        // LastPeer(1000) address
  int64_t peer_age_ms = 0;
  uint32_t online_target = 0;             // online::Target(), 0 when not joining by code
  bool have_lan = false;
  uint32_t lan_ip = 0, lan_mask = 0;
  bool broadcast_fallback = false;        // the send explicitly asks for the final fallback
};
// Host-order IPv4 destination, or nullopt when nothing is known (send nothing).
inline std::optional<uint32_t> ChooseDestination(const DestinationInputs& in) {
  if (!in.broadcast_fallback) {
    if (in.learned_host) return in.learned_host;
    if (in.peer_ip && in.peer_age_ms >= 0 && in.peer_age_ms < kPeerLiveMs) return in.peer_ip;
  }
  if (in.online_target) return in.online_target;
  if (in.have_lan) return in.lan_ip | ~in.lan_mask;
  return std::nullopt;
}

// Retry schedule shared by option snapshots and commands: first send at 0, resend at 1 s and 3 s (the last one
// uses the broadcast/online-target fallback), give up 1 s after the final resend.
enum class RetryAction { kNone, kResend, kResendFallback, kGiveUp };
class RetryTimer {
 public:
  explicit RetryTimer(int64_t first_send_ms) : first_(first_send_ms) {}
  // Call as time advances; each step fires once.
  RetryAction Poll(int64_t now_ms) {
    const int64_t t = now_ms - first_;
    if (step_ == 0 && t >= 1000) { step_ = 1; return RetryAction::kResend; }
    if (step_ == 1 && t >= 3000) { step_ = 2; return RetryAction::kResendFallback; }
    if (step_ == 2 && t >= 4000) { step_ = 3; return RetryAction::kGiveUp; }
    return RetryAction::kNone;
  }
  bool Finished() const { return step_ == 3; }
 private:
  int64_t first_;
  int step_ = 0;
};

// Remembers the latest 16 command outcomes by source IP, XUID and sequence so a duplicate gets the same answer.
struct Outcome { uint32_t ip = 0; uint64_t xuid = 0; uint32_t sequence = 0; bool refused = false; std::string text; };
class OutcomeCache {
 public:
  const Outcome* Find(uint32_t ip, uint64_t xuid, uint32_t seq) const {
    for (const auto& o : items_) if (o.ip == ip && o.xuid == xuid && o.sequence == seq) return &o;
    return nullptr;
  }
  void Remember(Outcome o) {
    items_.push_back(std::move(o));
    if (items_.size() > 16) items_.pop_front();
  }
  size_t size() const { return items_.size(); }
 private:
  std::deque<Outcome> items_;
};

// Host side: up to eight requesting IP/port pairs kept for 6 s after their latest request.
class MenuRequesters {
 public:
  void Note(uint32_t ip, uint16_t port, int64_t now_ms) {
    Expire(now_ms);
    for (auto& r : list_) if (r.ip == ip && r.port == port) { r.last = now_ms; return; }
    if (list_.size() < 8) list_.push_back({ip, port, now_ms});
  }
  std::vector<std::pair<uint32_t, uint16_t>> Active(int64_t now_ms) {
    Expire(now_ms);
    std::vector<std::pair<uint32_t, uint16_t>> v;
    for (const auto& r : list_) v.push_back({r.ip, r.port});
    return v;
  }
  void Clear() { list_.clear(); }
 private:
  struct R { uint32_t ip; uint16_t port; int64_t last; };
  void Expire(int64_t now) {
    for (size_t i = 0; i < list_.size();) {
      if (now - list_[i].last > 6000) list_.erase(list_.begin() + i); else ++i;
    }
  }
  std::vector<R> list_;
};

// A changed catalog or world state is pushed twice, 1 s apart; an emptied menu table is sent on change and once
// more about 1 s later; a non-empty menu table repeats every second.
class DoublePush {
 public:
  void Changed(int64_t now_ms) { due_ = now_ms; second_ = true; }
  // True when a push should go out now.
  bool Poll(int64_t now_ms) {
    if (due_ < 0 || now_ms < due_) return false;
    if (second_) { second_ = false; due_ = now_ms + 1000; return true; }
    due_ = -1;
    return true;
  }
 private:
  int64_t due_ = -1;
  bool second_ = false;
};

// Poll cadence: joiner every 2 frames, host every 4 frames once a game peer is live.
inline bool PollDue(uint64_t frame, bool host) { return frame % (host ? 4 : 2) == 0; }

}  // namespace bo2cs
