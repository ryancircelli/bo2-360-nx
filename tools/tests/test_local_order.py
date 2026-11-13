"""Compile the production ordering hook with an authored stock-swap transport."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / "games/bo2/src/bo2zm_local_order.cpp").read_text(encoding="utf-8")
    # Keep every production function body. Replace only hook/log/guest-call
    # plumbing, whose authored implementation is in the fixture.
    for include in ('#include <rex/hook.h>', '#include <rex/logging.h>', '#include "bo2zm_guest.h"'):
        source = source.replace(include, "")
    fixture = (ROOT / "tools/tests/local_order_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// PRODUCTION_LOCAL_ORDER", source)
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler and Path("C:/Program Files/LLVM/bin/clang++.exe").is_file():
        compiler = "C:/Program Files/LLVM/bin/clang++.exe"
    if not compiler:
        raise SystemExit("Set CXX to a C++23 compiler")
    with tempfile.TemporaryDirectory(prefix="bo2-local-order-") as directory:
        cpp = Path(directory) / "local_order.cpp"
        exe = Path(directory) / ("local_order.exe" if os.name == "nt" else "local_order")
        cpp.write_text(fixture, encoding="utf-8")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-msse4.1",
                        "-I" + str(ROOT / "sdk/include"), "-I" + str(ROOT / "sdk/thirdparty/simde"),
                        str(cpp), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
