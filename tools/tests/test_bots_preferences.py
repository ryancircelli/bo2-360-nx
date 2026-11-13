"""Compile the bot preference manager against minimal SDK boundaries; never runs the game."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "ppc.h": "#pragma once\nstruct PPCContext {};\n",
    "cvar.h": r"""#pragma once
#include <cstdint>
namespace rex::cvar { enum class Lifecycle { kInitOnly }; }
namespace test {
struct Registrar {
  Registrar range(int, int) { return {}; }
  Registrar lifecycle(rex::cvar::Lifecycle) { return {}; }
};
}
#define REXCVAR_DEFINE_INT32(name, value, ...) int32_t name = value; static auto name##_registration = test::Registrar{}
#define REXCVAR_GET(name) name
#define REXCVAR_ALIAS(alias, name) static_assert(true)
""",
    "logging.h": "#pragma once\n#define REXLOG_WARN(...) ((void)0)\n#define REXLOG_INFO(...) ((void)0)\n",
    "runtime.h": r"""#pragma once
#include <filesystem>
namespace rex {
struct Runtime {
  static Runtime* instance() { return nullptr; }
  std::filesystem::path user_data_root() const { return {}; }
};
}
""",
}


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++")
    if not compiler and Path("C:/msys64/ucrt64/bin/g++.exe").exists():
        compiler = "C:/msys64/ucrt64/bin/g++.exe"
    if not compiler:
        raise SystemExit("Set CXX to a C++20 compiler")
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="bo2-bots-tests-") as directory:
        temporary = Path(directory)
        includes = temporary / "include" / "rex"
        includes.mkdir(parents=True)
        for name, contents in STUBS.items():
            (includes / name).write_text(contents, encoding="utf-8")
        for campaign in (False, True):
            executable = temporary / ("campaign.exe" if campaign else "zombies.exe")
            command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-pthread",
                       "-I" + str(includes.parent), "-I" + str(ROOT / "core"), str(ROOT / "tools/tests/bots_preferences_test.cpp"),
                       str(ROOT / "games/bo2/src/bo2zm_bots.cpp"), str(ROOT / "core/bo2zm_settings.cpp"),
                       "-o", str(executable)]
            if campaign:
                command.append("-DBO2_GAME_CAMPAIGN=1")
            subprocess.run(command, check=True, env=env, cwd=ROOT)
            subprocess.run([str(executable), str(temporary / ("cp-user" if campaign else "zm-user"))],
                           check=True, env=env, cwd=ROOT)


if __name__ == "__main__":
    main()
