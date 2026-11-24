"""The native biquad's dot product must be bit-identical to the recompiled vmsum4fp128 (Xenon: exact products
summed in double in guest lane order, rounded once). Its float-summed form drifted from it and the checked
calls switched the native biquad off (MISMATCH at the filter state). --source shows the fixture failing
against an older bo2zm_native_audio.cpp.
"""
import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

FIXTURE = r"""
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <rex/ppc/intrinsics.h>
#include <simde/arm/neon.h>

// PRODUCTION

// vmsum4fp128 written out in guest lane order (lane i here is guest element i), not through VmxDot.
static uint32_t Reference(const float c[4], const float s[4]) {
  double p[4];
  for (int i = 0; i < 4; ++i) {
    uint32_t a, b;
    std::memcpy(&a, &c[i], 4);
    std::memcpy(&b, &s[i], 4);
    if ((a & 0x7F800000u) == 0) a &= 0x80000000u;
    if ((b & 0x7F800000u) == 0) b &= 0x80000000u;
    float af, bf;
    std::memcpy(&af, &a, 4);
    std::memcpy(&bf, &b, 4);
    p[i] = double(af) * double(bf);
  }
  const double sum = (p[0] + p[2]) + (p[1] + p[3]);
  const float r = float(sum);
  uint32_t bits;
  std::memcpy(&bits, &r, 4);
  if (std::isnan(r) || (std::isinf(r) && !std::isinf(sum))) return 0x7FC00000u;
  if ((bits & 0x7F800000u) == 0) bits &= 0x80000000u;
  return bits;
}

int main() {
  std::mt19937 rng(1124);
  std::uniform_real_distribution<float> coefficient(-2.0f, 2.0f), sample(-1.0f, 1.0f);
  int bad = 0;
  for (int n = 0; n < 1000000; ++n) {
    float c[4] = {coefficient(rng), coefficient(rng), coefficient(rng), coefficient(rng)};
    float s[4] = {sample(rng), sample(rng), sample(rng), sample(rng)};
    if (n % 4 == 0) s[3] = (c[0] * s[0] + c[1] * s[1] + c[2] * s[2]) / -c[3];  // near-cancelling, as a settled filter
    if (n == 1) s[2] = 1e-40f;                                                      // denormal input
    if (n == 2) { c[0] = c[1] = 3e38f; s[0] = s[1] = 3e38f; }                        // finite overflow
    alignas(16) float out[4];
    simde_vst1q_f32(out, Dot(simde_vld1q_f32(c), simde_vld1q_f32(s)));
    const uint32_t want = Reference(c, s);
    for (float lane : out) {
      uint32_t got;
      std::memcpy(&got, &lane, 4);
      if (got != want && bad++ < 5) std::printf("lane %08X, vmsum4fp128 %08X (case %d)\n", got, want, n);
    }
  }
  if (bad) std::printf("%d lanes differ\n", bad);
  return bad != 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "games/bo2/src/bo2zm_native_audio.cpp")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    fragments = []
    for pattern in (r"^using F4 = .*?;", r"^inline F4 Dot\(.*?^}"):
        match = re.search(pattern, source, re.M | re.S)
        if not match:
            raise SystemExit("production definition not found: " + pattern)
        fragments.append(match.group())
    program = FIXTURE.replace("// PRODUCTION", "\n".join(fragments))
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("Set CXX to a C++23 compiler")
    x86 = platform.machine().lower() in ("x86_64", "amd64")
    with tempfile.TemporaryDirectory(prefix="bo2-biquad-dot-") as directory:
        cpp = Path(directory) / "biquad_dot.cpp"
        exe = Path(directory) / ("biquad_dot.exe" if os.name == "nt" else "biquad_dot")
        cpp.write_text(program, encoding="utf-8")
        subprocess.run([compiler, "-std=c++23", "-O2", "-Wall", "-Wextra", "-Werror", *(["-msse4.1"] if x86 else []),
                        "-I" + str(ROOT / "sdk/include"), "-I" + str(ROOT / "sdk/thirdparty/simde"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
