"""Compile and exercise the actual preset/profile code without the game or SDK.

Run: python -I tools/tests/test_profile_scope.py [--source path/to/bo2zm_preset.cpp]
Needs a C++17 compiler (CXX, g++, or clang++). Only the standalone fixture is built.
The extracted declarations, functions and UI publication block are production text;
the fixture supplies settings storage, the mode answer and the console transport.
Startup policy uses the production helper and the SDK header's actual Source enum.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "games/bo2/src/bo2zm_preset.cpp")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    compiler = os.environ.get("CXX") or shutil.which("g++")
    if not compiler and Path("C:/msys64/ucrt64/bin/g++.exe").is_file():
        compiler = "C:/msys64/ucrt64/bin/g++.exe"
    compiler = compiler or shutil.which("clang++")
    if not compiler:
        parser.error("set CXX to a C++17 compiler")

    fragments = [
        between(source, "constexpr int kPresets", "constexpr uint32_t kDvarFlags"),
        between(source, "enum OptionId", "// The dvars, each with a value"),
        between(source, "std::atomic<int> g_index", "std::atomic<int> g_request"),
        between(source, "std::mutex g_map_mutex", "int64_t Ns("),
        between(source, "int LevelOf(", "void LoadCustom("),
        between(source, "struct Location {", "void LoadProfiles("),
        between(source, "void MapLoading(", "}  // namespace bo2zm::preset"),
        "void PublishProfileUI(PPCContext& ctx, uint8_t* base) {\n"
        + between(source, "  // MAP PROFILE (in a match:", "  // Model detail: how many models")
        + "}\n",
    ]
    fixture = (ROOT / "tools/tests/profile_scope_fixture.cpp").read_text(encoding="utf-8")
    cvar_header = (ROOT / "sdk/include/rex/cvar.h").read_text(encoding="utf-8")
    source_enum = between(cvar_header, "enum class Source", "// Validation constraints")
    program = fixture.replace("// CVAR_SOURCE_ENUM", "namespace rex::cvar {\n" + source_enum + "}\n")
    program = program.replace("// PRODUCTION_PROFILE_CODE", "\n".join(fragments))
    with tempfile.TemporaryDirectory(prefix="bo2-profile-test-") as directory:
        cpp = Path(directory) / "profile_scope.cpp"
        exe = Path(directory) / ("profile_scope.exe" if os.name == "nt" else "profile_scope")
        cpp.write_text(program, encoding="utf-8")
        env = os.environ.copy()
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
                       check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
