"""Compare Skin4 (app/src/bo2zm_native_skin.cpp) with the generated original sub_829143D0, offline.

The original is regex-extracted from the read-only generated code and compiled in a temporary directory with the
production skinning file; both run on identical synthetic guest memory and every byte of it, plus r3, is compared.
Usage: test_native_skin4.py [calls] [seed]        (defaults 120000, 1)
       test_native_skin4.py bench [vertices] [runs]  (-O3 -march=armv8-a; defaults 256, 15)
No game files, no game process.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NAME = "829143D0"
STRIPPED_INCLUDES = ('#include "bo2zm_game.h"', '#include "bo2zm_native_skin.h"', "#include <rex/cvar.h>",
                     "#include <rex/hook.h>", "#include <rex/logging.h>", '#include "bo2zm_benchmark.h"',
                     '#include "bo2zm_gpu_skin.h"')


def build(directory, flags):
    body = None
    for path in (ROOT / "app/generated/default_mp").glob("bo2zm_recomp.*.cpp"):
        match = re.search(r"DEFINE_REX_FUNC\(sub_" + NAME + r"\) \{.*?(?=\nDEFINE_REX_FUNC|\Z)",
                          path.read_text(encoding="utf-8"), re.S)
        if match:
            body = match.group()
    if body is None:
        raise SystemExit("sub_%s not found in the generated code" % NAME)
    dependencies = sorted(set(re.findall(r"__imp__sub_[0-9A-F]+(?=\(ctx, base\))", body)))
    if dependencies:
        raise SystemExit("the original calls %s: stub them in the fixture" % dependencies)
    production = (ROOT / "app/src/bo2zm_native_skin.cpp").read_text(encoding="utf-8")
    for include in STRIPPED_INCLUDES:
        production = production.replace(include, "")
    # RecordGpu (GPU job recording) is not part of the kernel; it needs the renderer's headers.
    production = re.sub(r"\nbool RecordGpu\(.*?\n}\n", "\n", production, count=1, flags=re.S)
    header = (ROOT / "app/src/bo2zm_native_skin.h").read_text(encoding="utf-8")
    declarations = re.search(r"namespace bo2zm::native_skin \{.*?\n}  // namespace", header, re.S)
    fixture = (ROOT / "tools/tests/native_skin4_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// PRODUCTION_SKIN", production).replace("// CONTROL_BODY", body)
    cpp = Path(directory) / "skin4.cpp"
    exe = Path(directory) / "skin4"
    cpp.write_text(fixture, encoding="utf-8")
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("Set CXX to a C++23 compiler")
    subprocess.run([compiler, "-std=c++23", *flags, "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-unused-variable",
                    "-Wno-unused-parameter", "-Wno-unused-function", "-DFMT_HEADER_ONLY",
                    "-I", str(ROOT / "app/src"), "-I", str(ROOT / "sdk/include"),
                    "-I", str(ROOT / "sdk/thirdparty/simde"), "-I", str(ROOT / "sdk/thirdparty/fmt/include"),
                    str(cpp), "-o", str(exe)], check=True)
    return exe


def main():
    args = sys.argv[1:]
    bench = bool(args) and args[0] == "bench"
    if bench:
        args = args[1:]
    with tempfile.TemporaryDirectory(prefix="bo2-native-skin4-") as directory:
        if bench:
            exe = build(directory, ["-O3", "-march=armv8-a"])
            vertices = int(args[0]) if args else 256
            runs = int(args[1]) if len(args) > 1 else 15
            raise SystemExit(subprocess.run([str(exe), "bench", str(vertices), str(runs)]).returncode)
        exe = build(directory, ["-O2"])
        calls = args[0] if args else "120000"
        seed = args[1] if len(args) > 1 else "1"
        result = subprocess.run([str(exe), "test", calls, seed])
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
