// Per-view cheat preferences and effective settings. No guest calls or implicit
// current-player state: callers select a player before touching this object.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <utility>

namespace bo2zm::cheats::detail {

constexpr int kLocalPlayers = 4;
enum class Option {
  God, Fly, Collision, Combo, ComboMode, Fov, Ammo, Explosive, NoRecoil,
  Ignore, KeepPerks, Jump, Feather, Hud, Third, ThirdCrosshair, CrosshairType,
  CrosshairColor, GivePap, CrossPerks, Speed, Cheats, Count
};
struct OptionSpec {
  std::string_view name;
  Option option;
  int maximum;
  int normal;
  bool gated;
};
inline constexpr OptionSpec kOptions[] = {
    {"god", Option::God, 1, 0, true}, {"fly", Option::Fly, 2, 0, true},
    {"collision", Option::Collision, 1, 0, false}, {"combo", Option::Combo, 1, 0, false},
    {"combo_mode", Option::ComboMode, 1, 0, false}, {"fov", Option::Fov, 11, 2, true},
    {"ammo", Option::Ammo, 1, 0, true}, {"explosive", Option::Explosive, 1, 0, true},
    {"norecoil", Option::NoRecoil, 1, 0, true}, {"zm_ignore", Option::Ignore, 1, 0, true},
    {"keep_perks", Option::KeepPerks, 1, 0, true}, {"jump", Option::Jump, 5, 0, true},
    {"feather", Option::Feather, 1, 0, true}, {"hud", Option::Hud, 1, 0, true},
    {"third", Option::Third, 1, 0, true}, {"third_crosshair", Option::ThirdCrosshair, 2, 1, false},
    {"tpp_type", Option::CrosshairType, 1, 0, false}, {"tpp_color", Option::CrosshairColor, 6, 0, false},
    {"give_pap", Option::GivePap, 1, 0, false}, {"cross_perks", Option::CrossPerks, 1, 0, false},
    {"speed", Option::Speed, 3, 0, true},
    {"cheats", Option::Cheats, 1, 0, false},
};
static_assert(std::size(kOptions) == static_cast<size_t>(Option::Count));
// The options a console that joined a System Link game sends to its host (bo2zm_cheat_sync.cpp), in the message's
// order: the ones the host's server applies to a player, and the player's CHEATS switch (last, so the values before
// it are already its effective ones). The view's own (field of view, HUD, crosshair), the fly combo (read from this
// console's pad) and the menu's preferences stay on the console that picked them; the joining console applies the
// movement ones to its own prediction too.
inline constexpr Option kSyncedOptions[] = {
    Option::God, Option::Fly, Option::Collision, Option::Jump, Option::Feather, Option::Ammo,
    Option::Explosive, Option::NoRecoil, Option::Ignore, Option::KeepPerks, Option::Third, Option::Speed,
    Option::Cheats,
};
constexpr int kSyncedCount = static_cast<int>(std::size(kSyncedOptions));
constexpr int OptionMaximum(Option option) { return kOptions[static_cast<size_t>(option)].maximum; }
constexpr std::string_view OptionName(Option option) { return kOptions[static_cast<size_t>(option)].name; }
static_assert([] {
  for (size_t i = 0; i < std::size(kOptions); ++i) {
    if (static_cast<size_t>(kOptions[i].option) != i) return false;
  }
  return true;
}());

class PlayerOptions {
 public:
  PlayerOptions() {
    for (const auto& spec : kOptions) {
      wanted_[Index(spec.option)] = spec.normal;
      effective_[Index(spec.option)] = spec.normal;
    }
  }
  int Effective(Option option) const {
    return effective_[Index(option)].load(std::memory_order_relaxed);
  }
  bool Get(std::string_view name, int& value) const {
    const bool fly = name == "noclip" || name == "ufo";
    const auto* spec = Find(fly ? "fly" : name);
    if (!spec || name == "fly") return false;
    value = wanted_[Index(spec->option)].load(std::memory_order_relaxed);
    if (fly) value = value == (name == "noclip" ? 1 : 2);
    return true;
  }
  bool Set(std::string_view name, int value) {
    const bool fly = name == "noclip" || name == "ufo";
    const auto* spec = Find(fly ? "fly" : name);
    if (!spec || name == "fly") return false;
    std::lock_guard lock(mutex_);
    if (fly) {
      const int mode = name == "noclip" ? 1 : 2;
      const int previous = wanted_[Index(Option::Fly)];
      value = value > 0 ? mode : previous == mode ? 0 : previous;
    }
    Store(*spec, value, false);
    return true;
  }
  // Programmatic test/benchmark setters retain their ability to enable a
  // primary-player cheat independently of the menu master switch.
  void Direct(Option option, int value) {
    std::lock_guard lock(mutex_);
    Store(kOptions[Index(option)], value, true);
  }
  void ResetGameplay() {
    std::lock_guard lock(mutex_);
    combo_presses_ = 0;
    for (const auto& spec : kOptions) {
      if (spec.gated) Store(spec, spec.normal, false);
    }
  }
  bool QueueCombo(bool aiming, bool pressed, uint64_t revision) {
    std::lock_guard lock(mutex_);
    if (!aiming || !Effective(Option::Cheats) || !Effective(Option::Combo)) return false;
    if (pressed) {
      if (combo_revision_ != revision) combo_presses_ = 0;
      combo_revision_ = revision;
      combo_presses_ = (combo_presses_ + 1) & 1;
    }
    return true;
  }
  int TakeCombo(uint64_t revision) {
    std::lock_guard lock(mutex_);
    const int presses = std::exchange(combo_presses_, 0);
    if (!presses || combo_revision_ != revision || !Effective(Option::Cheats) || !Effective(Option::Combo)) return -1;
    const int mode = Effective(Option::Fly) == 0 ? Effective(Option::ComboMode) + 1 : 0;
    Store(kOptions[Index(Option::Fly)], mode, false);
    return mode;
  }
  void CancelCombo() { std::lock_guard lock(mutex_); combo_presses_ = 0; }
  void CopyFrom(const PlayerOptions& other) {
    if (this == &other) return;
    std::scoped_lock lock(mutex_, other.mutex_);
    for (size_t i = 0; i < wanted_.size(); ++i) {
      wanted_[i].store(other.wanted_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
      effective_[i].store(other.effective_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }
  void ResetAll() {
    std::lock_guard lock(mutex_);
    combo_presses_ = 0;
    for (const auto& spec : kOptions) {
      wanted_[Index(spec.option)] = spec.normal;
      effective_[Index(spec.option)] = spec.normal;
    }
  }

 private:
  static constexpr size_t Index(Option option) { return static_cast<size_t>(option); }
  static const OptionSpec* Find(std::string_view name) {
    for (const auto& spec : kOptions) if (spec.name == name) return &spec;
    return nullptr;
  }
  void Store(const OptionSpec& spec, int value, bool direct) {
    if (spec.option == Option::Cheats || spec.option == Option::Combo ||
        spec.option == Option::ComboMode || spec.option == Option::Fly) combo_presses_ = 0;
    value = std::clamp(value, 0, spec.maximum);
    wanted_[Index(spec.option)].store(value, std::memory_order_relaxed);
    const bool enabled = wanted_[Index(Option::Cheats)].load(std::memory_order_relaxed) != 0;
    effective_[Index(spec.option)].store(
        direct || enabled || !spec.gated ? value : spec.normal, std::memory_order_relaxed);
    if (spec.option == Option::Cheats) {
      for (const auto& other : kOptions) {
        effective_[Index(other.option)].store(
            enabled || !other.gated ? wanted_[Index(other.option)].load(std::memory_order_relaxed) : other.normal,
            std::memory_order_relaxed);
      }
    }
  }
  mutable std::mutex mutex_;
  std::array<std::atomic<int>, static_cast<size_t>(Option::Count)> wanted_{};
  std::array<std::atomic<int>, static_cast<size_t>(Option::Count)> effective_{};
  int combo_presses_ = 0;  // guarded with master/fly settings by mutex_
  uint64_t combo_revision_ = 0;
};

}  // namespace bo2zm::cheats::detail
