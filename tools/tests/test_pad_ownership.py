"""Compile and exercise cross-process SDL pad ownership storage."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("A C++ compiler is required")
    sdl_include = Path(os.environ.get("SDL3_INCLUDE_DIR", "/opt/homebrew/include"))
    sdl_library = Path(os.environ.get("SDL3_LIBRARY", "/opt/homebrew/lib/libSDL3.dylib"))
    with tempfile.TemporaryDirectory(prefix="rex-pad-ownership-") as directory:
        temporary = Path(directory)
        executable = temporary / "pad_ownership_test"
        subprocess.run([
            compiler, "-std=c++23", "-I" + str(ROOT / "sdk/include"),
            str(ROOT / "tools/tests/pad_ownership_test.cpp"),
            str(ROOT / "sdk/src/input/sdl/pad_ownership.cpp"), "-o", str(executable),
        ], check=True, cwd=ROOT)
        subprocess.run([str(executable), str(temporary / "registry.txt")],
                       check=True, cwd=ROOT, timeout=10)
        registry = str(temporary / "concurrent-registry.txt")
        claim0 = subprocess.Popen([str(executable), "claim", "0", registry], cwd=ROOT)
        claim1 = subprocess.Popen([str(executable), "claim", "1", registry], cwd=ROOT)
        results = [claim0.wait(timeout=10), claim1.wait(timeout=10)]
        if sorted(results) != [0, 1]:
            raise AssertionError(f"exactly one instance must claim a device, got {results}")
        print("SDL pad identity, reconnect, and per-instance/global limits passed")

        if not (sdl_include / "SDL3/SDL.h").exists() or not sdl_library.exists():
            raise RuntimeError(
                "SDL3 development files are required for the virtual joystick test; "
                "set SDL3_INCLUDE_DIR and SDL3_LIBRARY"
            )
        virtual_executable = temporary / "sdl_virtual_pad_ownership_test"
        subprocess.run([
            compiler, "-std=c++23", "-I" + str(sdl_include),
            "-I" + str(ROOT / "sdk/include"),
            str(ROOT / "tools/tests/sdl_virtual_pad_ownership_test.cpp"),
            str(ROOT / "sdk/src/input/sdl/pad_ownership.cpp"),
            str(sdl_library), "-Wl,-rpath," + str(sdl_library.parent),
            "-o", str(virtual_executable),
        ], check=True, cwd=ROOT)
        subprocess.run([str(virtual_executable), str(temporary / "virtual-registry.txt")],
                       check=True, cwd=ROOT, timeout=10)
        print("Eight SDL virtual joystick ownership, reconnect, and limit checks passed")


if __name__ == "__main__":
    main()
