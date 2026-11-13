"""Test the actual RunLua boundary with mocked guest compilation and execution."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / "games/bo2/src/bo2zm_ui.cpp").read_text(encoding="utf-8")
    # Keep the production function bodies and constants. Only their external
    # guest entry points, logging and PPC context are supplied by the fixture.
    names = ("kGameLock", "kStateGlobal", "kGlobalCompilerSettings", "kGlobalSecurityMode",
             "kStateTop", "kStateStackBottom", "kTypeBoolean", "kTypeString")
    declarations = []
    for name in names:
        declarations.append(re.search(r"^constexpr uint32_t " + name + r" = .*?;", source, re.M).group())
    for name in ("g_attempts", "g_stack_relocations"):
        declarations.append(re.search(r"^uint32_t " + name + r" = .*?;", source, re.M).group())
    functions = []
    for name in ("Load32", "Store32", "StringOf", "RunLua"):
        functions.append(re.search(r"^[\w:]+ " + name + r"\([^\n]*\) \{.*?^\}", source, re.M | re.S).group())
    compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="bo2-ui-stack-") as directory:
        temporary = Path(directory)
        (temporary / "ui_stack_production.inc").write_text(
            "\n\n".join(declarations + functions), encoding="utf-8")
        executable = temporary / "ui_stack.exe"
        subprocess.run([compiler, "-std=c++23", "-I" + str(temporary),
                        str(ROOT / "tools/tests/ui_stack_restore_test.cpp"),
                        "-o", str(executable)], check=True, env=env, cwd=ROOT)
        subprocess.run([str(executable)], check=True, env=env, cwd=ROOT)


if __name__ == "__main__":
    main()
