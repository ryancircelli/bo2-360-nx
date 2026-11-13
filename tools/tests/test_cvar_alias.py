"""Build and run the cvar alias test (sdk/src/core/cvar.cpp, REXCVAR_ALIAS)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    third = ROOT / "sdk/thirdparty"
    with tempfile.TemporaryDirectory(prefix="bo2-cvar-alias-") as directory:
        executable = Path(directory) / "cvar_alias_test"
        subprocess.run([compiler, "-std=c++20", "-UNDEBUG", "-Wno-deprecated-literal-operator",
                        "-I" + str(ROOT / "tools/tests/cvar_alias_stub"), "-I" + str(ROOT / "sdk/include"),
                        "-I" + str(third / "tomlplusplus/include"), "-I" + str(third / "cli11/include"),
                        "-I" + str(third / "fmt/include"), "-DFMT_HEADER_ONLY",
                        str(ROOT / "tools/tests/cvar_alias_test.cpp"),
                        str(ROOT / "sdk/src/core/cvar.cpp"), "-o", str(executable)],
                       check=True, cwd=ROOT)
        subprocess.run([str(executable)], check=True, cwd=ROOT, timeout=30)


if __name__ == "__main__":
    main()
