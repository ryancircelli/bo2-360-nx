"""Exercise the actual SetBotCount/CurrentDvar bodies without running the game."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / "games/bo2/src/bo2zm_gsc_mods.cpp").read_text(encoding="utf-8")
    constants = []
    for name in ("kFindDvar", "kDvarString", "kRegisterInt", "kSetInt"):
        constants.append(re.search(r"^constexpr uint32_t " + name + r" = .*?;", source, re.M).group())
    functions = []
    for signature in ("std::string CurrentDvar(", "bool SetBotCount("):
        functions.append(re.search("^" + re.escape(signature) + r".*?^}", source, re.M | re.S).group())
    fixture = (ROOT / "tools/tests/bot_count_probe_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// PRODUCTION_DVAR_CONSTANTS", "\n".join(constants))
    fixture = fixture.replace("// PRODUCTION_BOT_DVAR_FUNCTIONS", "\n\n".join(functions))
    # Preserve the fixture's repository-relative include after moving its generated
    # translation unit into a temporary directory.
    fixture = fixture.replace('../../games/bo2/src/bo2zm_bots.h', (ROOT / 'games/bo2/src/bo2zm_bots.h').as_posix())
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler and Path("C:/Program Files/LLVM/bin/clang++.exe").is_file():
        compiler = "C:/Program Files/LLVM/bin/clang++.exe"
    if not compiler:
        raise SystemExit("Set CXX to a C++23 compiler")
    includes = [ROOT / "sdk/include", ROOT / "sdk/thirdparty/simde",
                ROOT / "sdk/thirdparty/fmt/include", ROOT / "sdk/thirdparty/spdlog/include"]
    with tempfile.TemporaryDirectory(prefix="bo2-four-bot-probe-") as directory:
        cpp = Path(directory) / "bot_count_probe.cpp"
        exe = Path(directory) / ("bot_count_probe.exe" if os.name == "nt" else "bot_count_probe")
        cpp.write_text(fixture, encoding="utf-8")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-msse4.1",
                        "-D_CRT_SECURE_NO_WARNINGS",
                        *["-I" + str(path) for path in includes], str(cpp), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
