"""The native biquad's dot product must be bit-identical to the vmsum4fp128 that the BO2 build's generated
sub_8258B208 runs: simde_mm_dp_ps(.., 0xFF) on lvx-loaded lanes (guest element i in host lane 3-i). A native
Dot using rex::ppc::VmxDot (double sum) differed from it once a filter settled, and the checked calls logged
MISMATCH at the filter state and switched the native biquad off. When the generated code is regenerated with
VmxDot, the natives must switch with it; the generated-source check below fails until they do.
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

// What the generated sub_8258B208 runs: lvx puts guest element i in host lane 3-i, then simde_mm_dp_ps(.., 0xFF).
static uint32_t Reference(const float c[4], const float s[4]) {
  const simde__m128 a = simde_mm_setr_ps(c[3], c[2], c[1], c[0]);
  const simde__m128 b = simde_mm_setr_ps(s[3], s[2], s[1], s[0]);
  uint32_t bits;
  const float r = simde_mm_cvtss_f32(simde_mm_dp_ps(a, b, 0xFF));
  std::memcpy(&bits, &r, 4);
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
      if (got != want && bad++ < 5) std::printf("lane %08X, generated %08X (case %d)\n", got, want, n);
    }
  }
  if (bad) std::printf("%d lanes differ\n", bad);
  return bad != 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "games/bo2/src/bo2zm_native_audio.cpp")
    parser.add_argument("--generated", type=Path, default=ROOT / "games/bo2/generated/default_mp/bo2zm_recomp.0.cpp")
    args = parser.parse_args()
    if args.generated.exists():
        body = re.search(r"^DEFINE_REX_FUNC\(sub_8258B208\).*?^}", args.generated.read_text(encoding="utf-8"), re.M | re.S)
        if not body or "simde_mm_dp_ps" not in body.group() or "VmxDot" in body.group():
            raise SystemExit("generated sub_8258B208 no longer uses simde_mm_dp_ps: switch the natives' dot products "
                             "(bo2zm_native_audio/cull/entities) to what it uses, then update this test's Reference")
    else:
        print("note: no generated code at", args.generated, "- checked Dot against dp_ps only")
    source = args.source.read_text(encoding="utf-8")
    fragments = []
    for pattern in (r"^using V = .*?;", r"^using F4 = .*?;", r"^inline F4 Dot\(.*?^}"):
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
