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
  // 1024-bit known answer from Python pow(); covers multi-word QWNE word order and carries.
  auto ka = unhex("464549f27b4b3855a6d782a1ff2277ff46afa6bc26bdca0c4b6969675d6b1f9b51d4a60032f770a672efc476e11ac11d13ee7829c12ec991ce88f312a6a039ecb545cfebef95c8bdf6a6cf683384877f6a4f600a6e47325e1e60afc74d4adf8b883a46859f79d5e79b21516c27d5007e00f1d26a8b38107be4b195295733d721");
  auto km = unhex("3b08efbeeb795c233e2255734e78b8c8a9520742eaaa5a31f6d3042815d151e9ada807b83508b5b88c29b9062518c1c52f97906ce79c32a3f9a72a3914221a023f53178bd2939bd1b6941a4810cc965dc758d78f1e747586536f7568c5f915a22302ec7d74b2a2e69ea8ea978a84e68a9ef6fae8d37bd2c1ea96b92c62296646");
  std::vector<uint8_t> kout(ka.size());
  if (!RsaPublicQwordNe(ka, kout, km, 65537) ||
      hex(kout) != "c69f230f9c67da5e3315b616e5f85b608cdec9bcec1493727d8f4086504c285e9b7060263aaffd34161138b932d5fcc00366a5927da053f475b53ba34fd755d6c56fe839a4b98b8af3333b975a341e8293550d07818071c390f1d1798431d1a2b659b3ac015ae1f7471327b939ce0d00d0e0111a0b9e57dc6f963dbea77f1852")
    return 5;
  std::cout << "PASS RSA synthetic vector, aliasing, range and invalid input checks\n";
}
