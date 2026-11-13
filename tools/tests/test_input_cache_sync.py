"""Exercise the production InputSystem with controlled native driver callbacks.

Only platform drivers, logging, profiling and cvar registration are replaced.
InputSystem, its header, assignments, state merging and guest ABI types are real.
Run with --negative-control to also require the pre-fix implementation to fail.
No SDK/game build or controller is needed.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "kernel.h": "#pragma once\n#include <rex/system/xtypes.h>\n",
    "ui/window.h": "#pragma once\nnamespace rex::ui { class Window {}; }\n",
    "dbg.h": "#pragma once\n#define SCOPE_profile_cpu_f(...) ((void)0)\n",
    "logging.h": "#pragma once\n",
    "input/flags.h": r'''#pragma once
#include <initializer_list>
#include <string>
struct FixtureString {
  std::string value;
  FixtureString& allowed(std::initializer_list<const char*>) { return *this; }
};
inline const std::string& FixtureGet(const FixtureString& value) { return value.value; }
inline bool FixtureGet(bool value) { return value; }
#define REXCVAR_DEFINE_STRING(name, value, ...) \
  FixtureString name{value}; auto name##_registration = name
#define REXCVAR_DEFINE_BOOL(name, value, ...) bool name = value
#define REXCVAR_GET(name) FixtureGet(name)
#define REXCVAR_ALIAS(alias, name) static_assert(true)
''',
}
BACKEND = r'''#pragma once
#include <rex/input/input_driver.h>
namespace rex::input::NAMESPACE {
class CLASS : public InputDriver {
 public:
  CLASS(rex::ui::Window* window, size_t order) : InputDriver(window, order) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>&) override {}
  X_RESULT GetDeviceState(DeviceId, X_INPUT_STATE*) override { return X_ERROR_DEVICE_NOT_CONNECTED; }
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_DEVICE_NOT_CONNECTED; }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override { return X_ERROR_EMPTY; }
};
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="bo2-input-cache-") as directory:
        temporary = Path(directory)
        includes = temporary / "include/rex"
        stubs = dict(STUBS)
        for namespace, name, folder in [
            ("mnk", "MnkInputDriver", "mnk"), ("nop", "NopInputDriver", "nop"),
            ("sdl", "SDLInputDriver", "sdl"), ("xinput", "XinputInputDriver", "xinput"),
            ("nx", "SwitchInputDriver", "switch"),
        ]:
            stubs[f"input/{folder}/{folder}_input_driver.h"] = (
                BACKEND.replace("NAMESPACE", namespace).replace("CLASS", name))
        for name, source in stubs.items():
            path = includes / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")

        def compile_and_run(label, source, override=None, negative=False):
            executable = temporary / (label + ".exe")
            command = [compiler, "-std=c++23", "-O1", "-g", "-pthread", "-DFMT_HEADER_ONLY",
                       "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
            if override:
                command += ["-I" + str(override)]
            command += ["-I" + str(includes.parent), "-I" + str(ROOT / "sdk/include"),
                        "-I" + str(ROOT / "sdk/thirdparty/fmt/include"),
                        str(ROOT / "tools/tests/input_cache_sync_test.cpp"), str(source),
                        str(ROOT / "sdk/src/input/device_assignment.cpp"),
                        str(ROOT / "sdk/src/input/state_merge.cpp"), "-o", str(executable)]
            subprocess.run(command, check=True, env=env, cwd=ROOT)
            result = subprocess.run([str(executable)] + (["--overlap-only"] if negative else []),
                                    env=env, cwd=ROOT, timeout=35, capture_output=True, text=True)
            print(label + ": " + result.stdout.strip(), flush=True)
            expected = 3 if negative else 0
            if result.returncode != expected:
                raise RuntimeError(f"{label}: exit {result.returncode}, expected {expected}\n{result.stderr}")

        if args.negative_control:
            baseline = temporary / "baseline/rex/input"
            baseline.mkdir(parents=True)
            for relative, path in [
                ("sdk/include/rex/input/input_system.h", baseline / "input_system.h"),
                ("sdk/src/input/input_system.cpp", temporary / "baseline.cpp"),
            ]:
                path.write_bytes(subprocess.check_output(["git", "show", "c4078e6:" + relative], cwd=ROOT))
            compile_and_run("unpatched", temporary / "baseline.cpp", temporary / "baseline", True)
        compile_and_run("patched", ROOT / "sdk/src/input/input_system.cpp")


if __name__ == "__main__":
    main()
