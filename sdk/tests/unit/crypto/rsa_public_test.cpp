#include "kernel/xboxkrnl/rsa_public.h"
#include <iostream>
#include <string>
#include <iomanip>
#include <sstream>
using rex::kernel::xboxkrnl::RsaPublicQwordNe;
std::vector<uint8_t> unhex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < s.size(); i += 2) out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  return out;
}
std::string hex(const std::vector<uint8_t>& bytes) {
  std::ostringstream out;
  for (auto b : bytes) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
  return out.str();
}
int main(int argc, char**) {
  if (argc > 1) {
    std::string a, m; uint32_t e;
    while (std::cin >> a >> m >> e) {
      auto in = unhex(a), mod = unhex(m); std::vector<uint8_t> out(in.size());
      std::cout << (RsaPublicQwordNe(in, out, mod, e) ? hex(out) : "FAIL") << '\n';
    }
    return 0;
  }
  auto in = unhex("0000000000000041"), m = unhex("0000000000000ca1");
  std::vector<uint8_t> out(8);
  if (!RsaPublicQwordNe(in, out, m, 17) || hex(out) != "0000000000000ae6") return 1;
  if (!RsaPublicQwordNe(in, in, m, 17) || in != out) return 2;
  if (RsaPublicQwordNe(m, out, m, 17) || RsaPublicQwordNe(in, out, m, 0)) return 3;
  if (RsaPublicQwordNe({}, {}, {}, 17)) return 4;
  std::cout << "PASS RSA synthetic vector, aliasing, range and invalid input checks\n";
}
