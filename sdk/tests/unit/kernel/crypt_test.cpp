#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl {
struct XECRYPT_SHA256_STATE;
void XeCryptSha224Init_entry(ppc_ptr_t<XECRYPT_SHA256_STATE>);
void XeCryptSha256Update_entry(ppc_ptr_t<XECRYPT_SHA256_STATE>, mapped_void, u32);
void XeCryptSha256Final_entry(ppc_ptr_t<XECRYPT_SHA256_STATE>, ppc_ptr_t<uint8_t>, u32);
void XeCryptBnDwLePkcs1Format_entry(mapped_void, u32, mapped_void, u32);
u32 XeCryptBnDwLePkcs1Verify_entry(mapped_void, mapped_void, u32);
u64 XeCryptBnQwNeModInv_entry(u64);
void XeCryptBnQwNeModMul_entry(mapped_u64, mapped_u64, mapped_u64, u64, mapped_u64, u32);
struct XECRYPT_MD5_STATE;
struct XECRYPT_HMACMD5_STATE;
void XeCryptMd5Init_entry(ppc_ptr_t<XECRYPT_MD5_STATE>);
void XeCryptMd5Update_entry(ppc_ptr_t<XECRYPT_MD5_STATE>, mapped_void, u32);
void XeCryptMd5Final_entry(ppc_ptr_t<XECRYPT_MD5_STATE>, mapped_void, u32);
void XeCryptHmacMd5Init_entry(ppc_ptr_t<XECRYPT_HMACMD5_STATE>, mapped_void, u32);
void XeCryptHmacMd5Update_entry(ppc_ptr_t<XECRYPT_HMACMD5_STATE>, mapped_void, u32);
void XeCryptHmacMd5Final_entry(ppc_ptr_t<XECRYPT_HMACMD5_STATE>, mapped_void, u32);
void XeCryptSha384Init_entry(ppc_ptr_t<uint8_t>);
void XeCryptSha384Update_entry(ppc_ptr_t<uint8_t>, mapped_void, u32);
void XeCryptSha384Final_entry(ppc_ptr_t<uint8_t>, ppc_ptr_t<uint8_t>, u32);
void XeCryptSha512Init_entry(ppc_ptr_t<uint8_t>);
void XeCryptSha512Update_entry(ppc_ptr_t<uint8_t>, mapped_void, u32);
void XeCryptSha512Final_entry(ppc_ptr_t<uint8_t>, ppc_ptr_t<uint8_t>, u32);

}
using namespace rex::kernel::xboxkrnl;

TEST_CASE("PKCS1 little endian blocks validate the hash and every padding byte", "[kernel][crypt]") {
  std::array<uint8_t, 20> hash;
  for (size_t i = 0; i < hash.size(); ++i) hash[i] = static_cast<uint8_t>(i);
  auto h = mapped_void::from_host(hash.data());
  std::array<uint8_t, 514> buffer;
  for (uint32_t size : {39u, 128u, 256u, 512u}) {
    for (uint32_t format = 0; format < 3; ++format) {
      buffer.fill(0xa5);
      auto output = mapped_void::from_host(buffer.data() + 1);
      XeCryptBnDwLePkcs1Format_entry(h, format, output, size);
      CHECK(buffer.front() == 0xa5);
      CHECK(buffer[size + 1] == 0xa5);
      CHECK(buffer[1] == 19);
      CHECK(buffer[20] == 0);
      CHECK(buffer[size - 1] == 1);
      CHECK(buffer[size] == 0);
      REQUIRE(XeCryptBnDwLePkcs1Verify_entry(h, output, size) == 1);
      for (size_t i = 1; i <= size; ++i) {
        buffer[i] ^= 0x80;
        CHECK(XeCryptBnDwLePkcs1Verify_entry(h, output, size) == 0);
        buffer[i] ^= 0x80;
      }
      hash[0] ^= 1;
      CHECK(XeCryptBnDwLePkcs1Verify_entry(h, output, size) == 0);
      hash[0] ^= 1;
    }
  }
  // Independent DER prefix (in forward order), encoded after the FF padding.
  const std::array<uint8_t, 15> prefix = {0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,
                                         0x03,0x02,0x1a,0x05,0x00,0x04,0x14};
  XeCryptBnDwLePkcs1Format_entry(h, 0, mapped_void::from_host(buffer.data()), 128);
  CHECK(std::equal(prefix.rbegin(), prefix.rend(), buffer.begin() + 20));
  for (uint32_t size : {0u, 38u, 513u}) {
    buffer.fill(0xa5);
    XeCryptBnDwLePkcs1Format_entry(h, 0, mapped_void::from_host(buffer.data()), size);
    CHECK(std::all_of(buffer.begin(), buffer.end(), [](auto x) { return x == 0xa5; }));
    CHECK(XeCryptBnDwLePkcs1Verify_entry(h, mapped_void::from_host(buffer.data()), size) == 0);
  }
  // In-place formatting preserves the digest before filling padding.
  std::copy(hash.begin(), hash.end(), buffer.begin());
  auto alias = mapped_void::from_host(buffer.data());
  XeCryptBnDwLePkcs1Format_entry(alias, 2, alias, 128);
  CHECK(XeCryptBnDwLePkcs1Verify_entry(h, alias, 128) == 1);
}

TEST_CASE("Montgomery inverse multiplies odd inputs to one modulo 2^64", "[kernel][crypt]") {
  for (uint64_t x : {1ull, 3ull, 17ull, 0xffffffffffffffffull, 0xffffffffffffffc5ull,
                     0x8000000000000001ull}) {
    CHECK(x * XeCryptBnQwNeModInv_entry(x) == 1);
  }
}

TEST_CASE("SHA224 shares the streaming SHA256 exports with its own IV", "[kernel][crypt]") {
  const std::array<std::pair<std::string, std::string>, 3> vectors = {{
    {"", "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f"},
    {"abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525"}
  }};
  for (const auto& [message, expected] : vectors) {
    alignas(16) std::array<uint8_t, 100> state;
    state.fill(0xa5);
    auto st = ppc_ptr_t<XECRYPT_SHA256_STATE>::from_host(
        reinterpret_cast<XECRYPT_SHA256_STATE*>(state.data()));
    XeCryptSha224Init_entry(st);
    for (char byte : message) {
      XeCryptSha256Update_entry(st, mapped_void::from_host(&byte), 1);
    }
    std::array<uint8_t, 29> digest;
    digest.fill(0xa5);
    XeCryptSha256Final_entry(st, ppc_ptr_t<uint8_t>::from_host(digest.data()), 28);
    CHECK(digest.back() == 0xa5);
    std::string hex;
    for (size_t i = 0; i < 28; ++i) {
      hex += "0123456789abcdef"[digest[i] >> 4];
      hex += "0123456789abcdef"[digest[i] & 15];
    }
    CHECK(hex == expected);
  }
}

TEST_CASE("Montgomery multiply matches independent integer oracle and aliases", "[kernel][crypt]") {
  {
    std::array<rex::be_u64, 1> a = {0x3e2255734e78b8c8ull};
    std::array<rex::be_u64, 1> b = {0xa9520742eaaa5a31ull};
    std::array<rex::be_u64, 1> m = {0xbb08efbeeb795c23ull};
    const std::array<uint64_t, 1> expected = {0x6ad46359b2fde68aull};
    std::array<rex::be_u64, 1> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 1> a = {0x3f53178bd2939bd1ull};
    std::array<rex::be_u64, 1> b = {0x536f7568c5f915a2ull};
    std::array<rex::be_u64, 1> m = {0xaf97906ce79c32a3ull};
    const std::array<uint64_t, 1> expected = {0x34f1b2eab092e0e1ull};
    std::array<rex::be_u64, 1> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 1> a = {0x9ea8ea978a84e68aull};
    std::array<rex::be_u64, 1> b = {0x9ef6fae8d37bd2c1ull};
    std::array<rex::be_u64, 1> m = {0xa302ec7d74b2a2e7ull};
    const std::array<uint64_t, 1> expected = {0x4c83971e0276a6d6ull};
    std::array<rex::be_u64, 1> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 1> a = {0xa6d782a1ff2277ffull};
    std::array<rex::be_u64, 1> b = {0x46afa6bc26bdca0cull};
    std::array<rex::be_u64, 1> m = {0xc64549f27b4b3855ull};
    const std::array<uint64_t, 1> expected = {0xbfa4881af75ed8ccull};
    std::array<rex::be_u64, 1> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 1);
    for (size_t i = 0; i < 1; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 2> a = {0x72efc476e11ac11dull, 0x13ee7829c12ec991ull};
    std::array<rex::be_u64, 2> b = {0xce88f312a6a039ecull, 0xb545cfebef95c8bdull};
    std::array<rex::be_u64, 2> m = {0x4b6969675d6b1f9bull, 0xd1d4a60032f770a6ull};
    const std::array<uint64_t, 2> expected = {0x24e7978ffa7ad371ull, 0x0400178aff46b3a4ull};
    std::array<rex::be_u64, 2> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 2> a = {0x1e60afc74d4adf8bull, 0x883a46859f79d5e7ull};
    std::array<rex::be_u64, 2> b = {0x9b21516c27d5007eull, 0x00f1d26a8b38107bull};
    std::array<rex::be_u64, 2> m = {0xf6a6cf683384877full, 0xea4f600a6e47325eull};
    const std::array<uint64_t, 2> expected = {0xd2ad2444ec8059d8ull, 0x4bd9287d95b2718aull};
    std::array<rex::be_u64, 2> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 2> a = {0xf0c8ebd828eac6a7ull, 0x77e1dcefa5e35b29ull};
    std::array<rex::be_u64, 2> b = {0x04019ade1b84c1ffull, 0x92224ae66c46957bull};
    std::array<rex::be_u64, 2> m = {0xe4b195295733d721ull, 0xcf97f9ca5493d37full};
    const std::array<uint64_t, 2> expected = {0xa65a1bfe14e58b2cull, 0xa27e2e3f68aba16dull};
    std::array<rex::be_u64, 2> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 2> a = {0x1e3acd213188577eull, 0x645f0a24a96bc5a0ull};
    std::array<rex::be_u64, 2> b = {0x2c2b3029dde6c8fcull, 0x735890c6386a4605ull};
    std::array<rex::be_u64, 2> m = {0x10521e555fc758dfull, 0xd74b4f92cdcb1aedull};
    const std::array<uint64_t, 2> expected = {0xa1951462c764d284ull, 0x55e4eaca5993eed4ull};
    std::array<rex::be_u64, 2> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 2);
    for (size_t i = 0; i < 2; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 4> a = {0x46d45e95c22f648dull, 0x3dfe0e007181561aull, 0x33f8c70abcfbe270ull, 0x10d201faaa767b6cull};
    std::array<rex::be_u64, 4> b = {0xafa73cb024dbdb18ull, 0xb5ecca81079a90dbull, 0x9331f4b36aed27e5ull, 0x4d9edeb91a0c3817ull};
    std::array<rex::be_u64, 4> m = {0xa626c7f017d05ab7ull, 0x1cb56f157d303bf0ull, 0x4598125bc757cebdull, 0x824c2c3b974ef6bdull};
    const std::array<uint64_t, 4> expected = {0xbcbbd3914fcd0d2eull, 0xaf9d342e51d1c48dull, 0x3d05d2cbd7a9005dull, 0x2fa54085c5556fb1ull};
    std::array<rex::be_u64, 4> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 4> a = {0x18b5cb95856786eeull, 0x46ba89fc2110fb46ull, 0x775a5de4807024e2ull, 0x9e0c804af051e0c7ull};
    std::array<rex::be_u64, 4> b = {0x4956faad207e66ebull, 0x0643690504a11087ull, 0xa90a97d452910f34ull, 0x20c7fce433712f44ull};
    std::array<rex::be_u64, 4> m = {0x014de73dd0c2f851ull, 0x8a389e72dc7c2bbeull, 0x03db21d05d1bd2c5ull, 0xac269b10903c9105ull};
    const std::array<uint64_t, 4> expected = {0xb4d21d4072467b4bull, 0x1f6a1d14c60e3844ull, 0x654549fda0c54008ull, 0x20dcfc24312b0a32ull};
    std::array<rex::be_u64, 4> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 4> a = {0x131cef3fa4bcd2f5ull, 0x65eceb1bff34f57dull, 0x1865b6ef95596667ull, 0x09f9035f41699692ull};
    std::array<rex::be_u64, 4> b = {0xd9f7db0bc9cd2afbull, 0xe9865884a1679ee0ull, 0x9d114a63fe0bfb64ull, 0x3a45ce8cc42efb5bull};
    std::array<rex::be_u64, 4> m = {0xbb655a98d11aa6ebull, 0x4ac297a6410bc94dull, 0x6a542968e07cde4cull, 0x878e175462aaef0dull};
    const std::array<uint64_t, 4> expected = {0x3570835fa55a9d05ull, 0x1e84c497cae610d0ull, 0x7963a4582ab78476ull, 0x70c144409e386b23ull};
    std::array<rex::be_u64, 4> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
  {
    std::array<rex::be_u64, 4> a = {0xc10cf7a6e7b3bb1cull, 0x1453c50e829481cfull, 0x288e6e794bddcd3full, 0x44570adf29df69b1ull};
    std::array<rex::be_u64, 4> b = {0x588dbf1e204fe4aeull, 0x53901472c4029a42ull, 0xeb29e3aa14f4a8baull, 0x16a039396343286eull};
    std::array<rex::be_u64, 4> m = {0x150214c4de3cf91dull, 0x4b43c88a6906efc5ull, 0x4a238c713a0167e0ull, 0x8226262ddf1f2205ull};
    const std::array<uint64_t, 4> expected = {0xf0fec3a9d4f97580ull, 0x816d8439c210c9a2ull, 0xe84866be13618adcull, 0x3316113fb0fd7591ull};
    std::array<rex::be_u64, 4> out;
    const auto inverse = XeCryptBnQwNeModInv_entry(m[0]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(out.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(out[i]) == expected[i]);
    XeCryptBnQwNeModMul_entry(mapped_u64::from_host(a.data()), mapped_u64::from_host(b.data()),
        mapped_u64::from_host(a.data()), inverse, mapped_u64::from_host(m.data()), 4);
    for (size_t i = 0; i < 4; ++i) CHECK(uint64_t(a[i]) == expected[i]);
  }
}

TEST_CASE("MD5 streaming uses exactly the 84 byte guest state", "[kernel][crypt]") {
  alignas(8) std::array<uint8_t, 92> block;
  block.fill(0xa5);
  auto st = ppc_ptr_t<XECRYPT_MD5_STATE>::from_host(
      reinterpret_cast<XECRYPT_MD5_STATE*>(block.data() + 4));
  XeCryptMd5Init_entry(st);
  const std::array<uint8_t, 8> initial = {0,0,0,0, 0x67,0x45,0x23,0x01};
  CHECK(std::equal(initial.begin(), initial.end(), block.begin() + 4));
  char message[] = "abc";
  XeCryptMd5Update_entry(st, mapped_void::from_host(message), 1);
  XeCryptMd5Update_entry(st, mapped_void::from_host(message + 1), 2);
  CHECK(block[7] == 3);
  const std::array<uint8_t, 16> expected = {0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
                                          0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
  std::array<uint8_t, 17> digest;
  digest.fill(0xa5);
  XeCryptMd5Final_entry(st, mapped_void::from_host(digest.data()), 16);
  CHECK(std::equal(expected.begin(), expected.end(), digest.begin()));
  CHECK(digest.back() == 0xa5);
  for (size_t i : {0u,1u,2u,3u,88u,89u,90u,91u}) CHECK(block[i] == 0xa5);
}

TEST_CASE("HMAC MD5 streaming uses exactly the 168 byte guest state", "[kernel][crypt]") {
  alignas(8) std::array<uint8_t, 176> block;
  block.fill(0xa5);
  auto st = ppc_ptr_t<XECRYPT_HMACMD5_STATE>::from_host(
      reinterpret_cast<XECRYPT_HMACMD5_STATE*>(block.data() + 4));
  std::array<uint8_t, 16> key;
  key.fill(0x0b);
  XeCryptHmacMd5Init_entry(st, mapped_void::from_host(key.data()), key.size());
  CHECK(block[7] == 64);
  CHECK(block[91] == 64);
  char message[] = "Hi There";
  XeCryptHmacMd5Update_entry(st, mapped_void::from_host(message), 3);
  XeCryptHmacMd5Update_entry(st, mapped_void::from_host(message + 3), 5);
  std::array<uint8_t, 17> digest;
  digest.fill(0xa5);
  XeCryptHmacMd5Final_entry(st, mapped_void::from_host(digest.data()), 16);
  const std::array<uint8_t, 16> expected = {0x92,0x94,0x72,0x7a,0x36,0x38,0xbb,0x1c,
                                          0x13,0xf4,0x8e,0xf8,0x15,0x8b,0xfc,0x9d};
  CHECK(std::equal(expected.begin(), expected.end(), digest.begin()));
  CHECK(digest.back() == 0xa5);
  for (size_t i : {0u,1u,2u,3u,172u,173u,174u,175u}) CHECK(block[i] == 0xa5);
}

TEST_CASE("SHA384 guest layout and incremental digest", "[kernel][crypt]") {
  std::array<uint8_t, 202> block;
  block.fill(0xa5);
  auto st = ppc_ptr_t<uint8_t>::from_host(block.data() + 1);
  XeCryptSha384Init_entry(st);
  const std::array<uint8_t, 8> iv = {0xcb,0xbb,0x9d,0x5d,0xc1,0x05,0x9e,0xd8};
  CHECK(std::equal(iv.begin(), iv.end(), block.begin() + 9));
  char message[] = "abc";
  XeCryptSha384Update_entry(st, mapped_void::from_host(message), 1);
  XeCryptSha384Update_entry(st, mapped_void::from_host(message + 1), 2);
  CHECK(block[4] == 3);
  std::array<uint8_t, 49> digest;
  digest.fill(0xa5);
  XeCryptSha384Final_entry(st, ppc_ptr_t<uint8_t>::from_host(digest.data()), 48);
  std::string hex;
  for (size_t i = 0; i < 48; ++i) {
    hex += "0123456789abcdef"[digest[i] >> 4];
    hex += "0123456789abcdef"[digest[i] & 15];
  }
  CHECK(hex == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
  CHECK(digest.back() == 0xa5);
  CHECK(block.front() == 0xa5);
  CHECK(block.back() == 0xa5);
}

TEST_CASE("SHA512 guest layout and incremental digest", "[kernel][crypt]") {
  std::array<uint8_t, 202> block;
  block.fill(0xa5);
  auto st = ppc_ptr_t<uint8_t>::from_host(block.data() + 1);
  XeCryptSha512Init_entry(st);
  const std::array<uint8_t, 8> iv = {0x6a,0x09,0xe6,0x67,0xf3,0xbc,0xc9,0x08};
  CHECK(std::equal(iv.begin(), iv.end(), block.begin() + 9));
  char message[] = "abc";
  XeCryptSha512Update_entry(st, mapped_void::from_host(message), 1);
  XeCryptSha512Update_entry(st, mapped_void::from_host(message + 1), 2);
  CHECK(block[4] == 3);
  std::array<uint8_t, 65> digest;
  digest.fill(0xa5);
  XeCryptSha512Final_entry(st, ppc_ptr_t<uint8_t>::from_host(digest.data()), 64);
  std::string hex;
  for (size_t i = 0; i < 64; ++i) {
    hex += "0123456789abcdef"[digest[i] >> 4];
    hex += "0123456789abcdef"[digest[i] & 15];
  }
  CHECK(hex == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  CHECK(digest.back() == 0xa5);
  CHECK(block.front() == 0xa5);
  CHECK(block.back() == 0xa5);
}
