"""Build and run the title manifest reader test (sdk/src/core/game_manifest.cpp, games/bo2/game.toml)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    toml = ROOT / "sdk/thirdparty/tomlplusplus/include"
    with tempfile.TemporaryDirectory(prefix="bo2-game-manifest-") as directory:
        executable = Path(directory) / "game_manifest_test"
        subprocess.run([compiler, "-std=c++20", "-UNDEBUG", "-Wno-deprecated-literal-operator",
                        "-I" + str(ROOT / "sdk/include"), "-I" + str(toml), "-I" + str(ROOT / "sdk/thirdparty/fmt/include"), "-DFMT_HEADER_ONLY",
                        str(ROOT / "tools/tests/game_manifest_test.cpp"),
                        str(ROOT / "sdk/src/core/game_manifest.cpp"), "-o", str(executable)],
                       check=True, cwd=ROOT)
        subprocess.run([str(executable), str(ROOT / "games/bo2/game.toml"),
                        str(ROOT / "sdk/src/graphics/metal/metal_flags.cpp")], check=True, cwd=ROOT, timeout=30)


if __name__ == "__main__":
    main()
