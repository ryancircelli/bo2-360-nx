"""Compare the production native math functions with read-only generated control bodies.

Usage: test_native_math.py [--bench] [--calls N] [--image PATH]
Controls are compiled only in a temporary directory. No game process is started.
The constant tables are read from the decrypted game image when present (else recorded values).
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = ("8289B6F0", "8248B820", "829170B0", "82917190", "829160B8")
DEFAULT_IMAGE = Path.home() / "Games/Xbox360/bo2-build/work/dump_tu18/default_mp.image.bin"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--calls", type=int, default=1000000)
    ap.add_argument("--image", default=str(DEFAULT_IMAGE))
    args = ap.parse_args()
    controls = {}
    for path in (ROOT / "app/generated/default_mp").glob("bo2zm_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8")
        for name in FUNCTIONS:
            match = re.search(r"DEFINE_REX_FUNC\(sub_" + name + r"\) \{.*?(?=\nDEFINE_REX_FUNC|\Z)", text, re.S)
            if match:
                controls[name] = match.group()
    if set(controls) != set(FUNCTIONS):
        raise SystemExit("The five generated control functions are required")
    bodies = "\n".join(controls[n] for n in FUNCTIONS)
    if re.search(r"__imp__sub_\w+\(ctx, base\)", bodies):
        raise SystemExit("A control calls another function; the harness has no stubs for that")
    fixture = (ROOT / "tools/tests/native_math_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// CONTROL_BODIES", bodies)
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("Set CXX to a C++23 compiler")
    opt = ["-O3", "-march=armv8-a"] if args.bench else ["-O2"]
    with tempfile.TemporaryDirectory(prefix="bo2-native-math-") as directory:
        cpp = Path(directory) / "native_math.cpp"
        exe = Path(directory) / "native_math"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([compiler, "-std=c++23", *opt, "-ffp-contract=off",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-parameter",
                        "-Wno-unused-but-set-variable", "-Wno-unused-function",
                        "-I" + str(ROOT / "sdk/include"), "-I" + str(ROOT / "sdk/thirdparty/simde"),
                        "-I" + str(ROOT / "app/src"), str(cpp), "-o", str(exe)], check=True)
        cmd = [str(exe), args.image if Path(args.image).is_file() else "", "bench" if args.bench else "test", str(args.calls)]
        subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
