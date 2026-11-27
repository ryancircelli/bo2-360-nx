// MD5 and HMAC-MD5 for XeCryptMd5* / XeCryptHmacMd5*. Written for this project from RFC 1321 and
// RFC 2104. The state is a plain host-order struct; titles only pass the block back to us.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rex::kernel::xboxkrnl {

struct Md5 {
  uint32_t state[4];
  uint64_t count;  // bytes hashed so far
  uint8_t buffer[64];

  void Init() {
    static const uint32_t kIv[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::memcpy(state, kIv, sizeof(state));
    count = 0;
    std::memset(buffer, 0, sizeof(buffer));
  }

  void Update(const uint8_t* data, size_t size) {
    while (size) {
      const size_t used = count % 64;
      const size_t n = std::min(size, 64 - used);
      std::memcpy(buffer + used, data, n);
      count += n;
      data += n;
      size -= n;
      if (count % 64 == 0) Block(buffer);
    }
  }

  // Writes min(out_size, 16) digest bytes. Leaves this state finished.
  void Final(uint8_t* out, size_t out_size) {
    const uint64_t bits = count * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (count % 64 != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (8 * i));
    Update(len, 8);
    uint8_t digest[16];
    for (int i = 0; i < 16; ++i) digest[i] = static_cast<uint8_t>(state[i / 4] >> (8 * (i % 4)));
    std::memcpy(out, digest, std::min<size_t>(out_size, 16));
  }

 private:
  void Block(const uint8_t* p) {
    static const uint8_t kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
      m[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | uint32_t{p[4 * i + 3]} << 24;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64; ++i) {
      uint32_t f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
      else { f = c ^ (b | ~d); g = (7 * i) % 16; }
      f += a + kK[i] + m[g];
      a = d; d = c; c = b;
      b += (f << kShift[i]) | (f >> (32 - kShift[i]));
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  }
};

struct HmacMd5 {
  Md5 inner, outer;

  void Init(const uint8_t* key, size_t key_size) {
    uint8_t k[64] = {};
    if (key_size > 64) {
      Md5 h;
      h.Init();
      h.Update(key, key_size);
      h.Final(k, 16);
    } else if (key_size) {
      std::memcpy(k, key, key_size);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5C;
    }
    inner.Init();
    inner.Update(ipad, 64);
    outer.Init();
    outer.Update(opad, 64);
  }
  void Update(const uint8_t* data, size_t size) { inner.Update(data, size); }
  void Final(uint8_t* out, size_t out_size) {
    uint8_t d[16];
    inner.Final(d, 16);
    outer.Update(d, 16);
    outer.Final(out, out_size);
  }
};

}  // namespace rex::kernel::xboxkrnl
