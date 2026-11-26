// Portable public-exponent modular arithmetic. QWNE uses least-significant
// 64-bit words first, with big-endian bytes within each word. No private-key use.
#pragma once
#include <cstdint>
#include <cstddef>
#include <utility>
#include <span>
#include <vector>

namespace rex::kernel::xboxkrnl {
inline bool RsaPublicQwordNe(std::span<const uint8_t> input, std::span<uint8_t> output,
                             std::span<const uint8_t> modulus, uint32_t exponent) {
  if (input.empty() || input.size() % 8 || input.size() > 1024 ||
      input.size() != output.size() || input.size() != modulus.size() || !exponent)
    return false;
  using Number = std::vector<uint32_t>;
  const size_t n = input.size() / 4;
  auto read = [n](std::span<const uint8_t> bytes) {
    Number a(n);
    for (size_t i = 0; i < n / 2; ++i) {
      uint64_t word = 0;
      for (size_t j = 0; j < 8; ++j) word = (word << 8) | bytes[i * 8 + j];
      a[2 * i] = uint32_t(word);
      a[2 * i + 1] = uint32_t(word >> 32);
    }
    return a;
  };
  auto less = [n](const Number& a, const Number& b) {
    for (size_t i = n; i-- > 0;) {
      if (a[i] != b[i]) return a[i] < b[i];
    }
    return false;
  };
  Number m = read(modulus), a = read(input), result(n);
  result[0] = 1;
  if (!less(result, m) || !less(a, m)) return false;
  auto add = [n, &m, &less](Number x, const Number& y) {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
      uint64_t sum = uint64_t(x[i]) + y[i] + carry;
      x[i] = uint32_t(sum);
      carry = sum >> 32;
    }
    if (carry || !less(x, m)) {
      uint64_t borrow = 0;
      for (size_t i = 0; i < n; ++i) {
        uint64_t rhs = uint64_t(m[i]) + borrow;
        borrow = uint64_t(x[i]) < rhs;
        x[i] = uint32_t(uint64_t(x[i]) - rhs);
      }
    }
    return x;
  };
  auto multiply = [n, &add](Number x, const Number& y) {
    Number product(n);
    for (size_t bit = 0; bit < n * 32; ++bit) {
      if ((y[bit / 32] >> (bit % 32)) & 1) product = add(std::move(product), x);
      x = add(x, x);
    }
    return product;
  };
  while (exponent) {
    if (exponent & 1) result = multiply(result, a);
    exponent >>= 1;
    if (exponent) a = multiply(a, a);
  }
  for (size_t i = 0; i < n / 2; ++i) {
    uint64_t word = uint64_t(result[2 * i]) | (uint64_t(result[2 * i + 1]) << 32);
    for (size_t j = 0; j < 8; ++j) output[i * 8 + j] = uint8_t(word >> (56 - j * 8));
  }
  return true;
}
}  // namespace rex::kernel::xboxkrnl
