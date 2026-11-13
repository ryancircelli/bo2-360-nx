"""Exercise the script pad and SDK routing without a full build or guest process."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
# Keep the real input driver ABI, guest structs, assignment and merger. Replace
# only unrelated runtime/UI/cvar/logging infrastructure in this small executable.
STUBS = {
    "runtime.h": """#pragma once
#include <functional>
#include <memory>
#include <rex/system/interfaces/input.h>
namespace rex { struct RuntimeConfig {
  std::function<std::unique_ptr<system::IInputSystem>(bool)> input_factory;
}; }
""",
    "kernel.h": '#pragma once\n#include <rex/system/xtypes.h>\n',
    "ui/window.h": '#pragma once\nnamespace rex::ui { class Window; }\n',
    "logging.h": """#pragma once
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
""",
    "cvar.h": """#pragma once
#include <string>
namespace rex::cvar {
enum class Lifecycle { kInitOnly };
struct Registrar {
  Registrar& lifecycle(Lifecycle) { return *this; }
  Registrar& range(int, int) { return *this; }
};
}
#define TEST_CVAR(type, name, value) \\
  inline type FLAGS_##name = value; \\
  static auto reg_##name = rex::cvar::Registrar{}
#define REXCVAR_DEFINE_STRING(name, value, ...) TEST_CVAR(std::string, name, value)
#define REXCVAR_DEFINE_INT32(name, value, ...) TEST_CVAR(int, name, value)
#define REXCVAR_GET(name) (FLAGS_##name)
#define REXCVAR_ALIAS(alias, name) static_assert(true)
""",
}


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="bo2-test-pad-") as directory:
        temporary = Path(directory)
        includes = temporary / "include"
        for name, source in STUBS.items():
            file = includes / "rex" / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(source, encoding="utf-8")
        # Compile the unchanged production InputSystem methods. Exclude only the
        # platform driver factory, whose SDL/XInput/window dependencies need a
        # full SDK build. No routing or query bodies are replaced by fixtures.
        source = (ROOT / "sdk/src/input/input_system.cpp").read_text(encoding="utf-8")
        start = source.index("namespace rex::input {")
        end = source.index("std::unique_ptr<InputSystem> CreateDefaultInputSystem(", start)
        sdk_queries = temporary / "input_queries.cpp"
        sdk_queries.write_text("""#include <chrono>
#include <algorithm>
#include <cmath>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#define SCOPE_profile_cpu_f(...) ((void)0)
#define REXCVAR_GET(name) false
""" + source[start:end] + "\n}\n", encoding="utf-8")
        executable = temporary / "test_pad.exe"
        subprocess.run([compiler, "-std=c++23", "-pthread",
                        "-I" + str(includes), "-I" + str(ROOT / "sdk/include"),
                        str(ROOT / "tools/tests/test_pad_test.cpp"), str(sdk_queries),
                        str(ROOT / "sdk/src/input/device_assignment.cpp"),
                        str(ROOT / "sdk/src/input/state_merge.cpp"),
                        "-o", str(executable)], check=True, env=env, cwd=ROOT)
        subprocess.run([str(executable)], check=True, env=env, cwd=ROOT, timeout=30)
        assignment = temporary / "test_pad_assignment.exe"
        subprocess.run([compiler, "-std=c++23", "-I" + str(ROOT / "sdk/include"),
                        str(ROOT / "tools/tests/test_pad_assignment.cpp"),
                        str(ROOT / "sdk/src/input/device_assignment.cpp"), "-o", str(assignment)],
                       check=True, env=env, cwd=ROOT)
        subprocess.run([str(assignment)], check=True, env=env, cwd=ROOT, timeout=10)
        print("script pad: synthetic-first physical/secondary assignment and sparse reconnect passed")


if __name__ == "__main__":
    main()
