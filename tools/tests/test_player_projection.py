"""Compare production projection hooks with read-only generated control bodies.

The controls are compiled only in a temporary directory, never committed.
Fixtures contain synthetic state, not game assets. No game process is started.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = ("821AFC90", "82687AF8", "82687B80", "82687D98")


def main():
    generated = ROOT / "app/generated/default_mp"
    controls = {}
    for path in generated.glob("bo2zm_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8")
        for name in FUNCTIONS:
            match = re.search(r"DEFINE_REX_FUNC\(sub_" + name + r"\) \{.*?(?=\nDEFINE_REX_FUNC|\Z)", text, re.S)
            if match:
                controls[name] = match.group().replace("{", f"{{ ++control_calls[{FUNCTIONS.index(name)}];", 1)
    if set(controls) != set(FUNCTIONS):
        raise SystemExit("The four generated control functions are required")
    bodies = "\n".join(controls[name] for name in FUNCTIONS)
    dependencies = sorted(set(re.findall(r"(__imp__sub_([0-9A-F]+))\(ctx, base\)", bodies)))
    stubs = "\n".join(
        f"void {name}(PPCContext& ctx, uint8_t* base) {{ Fake(0x{address}, ctx, base); }}"
        for name, address in dependencies)
    production = (ROOT / "games/bo2/src/bo2zm_player_projection.cpp").read_text(encoding="utf-8")
    for include in ('#include <rex/hook.h>', '#include "bo2zm_cheats.h"', '#include "bo2zm_guest.h"'):
        production = production.replace(include, "")
    fixture = (ROOT / "tools/tests/player_projection_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// CONTROL_BODIES", stubs + "\n" + bodies)
    fixture = fixture.replace("// PRODUCTION_PROJECTION", production)
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler and Path("C:/Program Files/LLVM/bin/clang++.exe").is_file():
        compiler = "C:/Program Files/LLVM/bin/clang++.exe"
    if not compiler:
        raise SystemExit("Set CXX to a C++23 compiler")
    with tempfile.TemporaryDirectory(prefix="bo2-player-projection-") as directory:
        cpp = Path(directory) / "projection.cpp"
        exe = Path(directory) / ("projection.exe" if os.name == "nt" else "projection")
        cpp.write_text(fixture, encoding="utf-8")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++23", "-O2", "-ffp-contract=off", "-msse4.1",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-parameter",
                        "-I" + str(ROOT / "sdk/include"), "-I" + str(ROOT / "sdk/thirdparty/simde"),
                        str(cpp), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
