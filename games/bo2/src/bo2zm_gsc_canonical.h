// Host storage for additional canonical field names, preserving stock IDs.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bo2zm::gsc {
// The caller serializes this registry with the guest's canonical count and
// stock registrar. No pointer is added beyond the stock guest array.
class CanonicalNames {
 public:
  static constexpr uint32_t kStockLimit = 13500;
  static constexpr uint32_t kLimit = 16384;

  void Reset() { names_.clear(); next_ = 0; }
  uint32_t Count() const { return next_; }

  template <class StockName>
  uint16_t Resolve(uint32_t count, const std::string& name, StockName stock_name) {
    if (count < kStockLimit || count > kLimit)
      throw std::runtime_error("invalid extended canonical-name count");
    if (next_ != count) {
      if (count != kStockLimit)
        throw std::runtime_error("canonical-name registry is out of sync");
      std::unordered_map<std::string, uint16_t> seeded;
      seeded.reserve(kLimit);
      for (uint32_t i = 1; i < count; ++i) seeded.emplace(stock_name(i), uint16_t(i));
      names_.swap(seeded);
      next_ = count;
    }
    const auto found = names_.find(name);
    if (found != names_.end()) return found->second;
    if (next_ == kLimit) throw std::runtime_error("extended canonical-name table is full");
    const auto id = uint16_t(next_);
    names_.emplace(name, id);
    ++next_;
    return id;
  }

 private:
  std::unordered_map<std::string, uint16_t> names_;
  uint32_t next_ = 0;
};
}  // namespace bo2zm::gsc
