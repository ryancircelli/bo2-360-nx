// MD5 / HMAC-MD5 known-answer checks (RFC 1321 A.5, RFC 2202 cases 1-2). Exit 0 = pass.
#include <cstdio>
#include <cstring>
#include <string>

#include "md5.h"

using namespace rex::kernel::xboxkrnl;

static std::string Hex(const uint8_t* d) {
  char s[33];
  for (int i = 0; i < 16; ++i) std::snprintf(s + 2 * i, 3, "%02x", d[i]);
  return s;
}

static int fails = 0;
static void Check(const char* name, const std::string& got, const char* want) {
  if (got != want) {
    std::printf("FAIL %s: %s != %s\n", name, got.c_str(), want);
    ++fails;
  }
}

int main() {
  uint8_t d[16];
  Md5 m;
  m.Init();
  m.Final(d, 16);
  Check("empty", Hex(d), "d41d8cd98f00b204e9800998ecf8427e");
  const char* abc = "abc";
  m.Init();
  m.Update(reinterpret_cast<const uint8_t*>(abc), 3);
  m.Final(d, 16);
  Check("abc", Hex(d), "900150983cd24fb0d6963f7d28e17f72");
  const char* long_in = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
  m.Init();  // split update across a block boundary
  m.Update(reinterpret_cast<const uint8_t*>(long_in), 37);
  m.Update(reinterpret_cast<const uint8_t*>(long_in) + 37, std::strlen(long_in) - 37);
  m.Final(d, 16);
  Check("80digits", Hex(d), "57edf4a22be3c955ac49da2e2107b67a");
  uint8_t key[16];
  std::memset(key, 0x0b, 16);
  HmacMd5 h;
  h.Init(key, 16);
  h.Update(reinterpret_cast<const uint8_t*>("Hi There"), 8);
  h.Final(d, 16);
  Check("hmac1", Hex(d), "9294727a3638bb1c13f48ef8158bfc9d");
  h.Init(reinterpret_cast<const uint8_t*>("Jefe"), 4);
  h.Update(reinterpret_cast<const uint8_t*>("what do ya want "), 16);
  h.Update(reinterpret_cast<const uint8_t*>("for nothing?"), 12);
  h.Final(d, 16);
  Check("hmac2", Hex(d), "750c783e6ab0b503eaa86e310a5db738");
  if (!fails) std::puts("md5_selfcheck ok");
  return fails;
}
