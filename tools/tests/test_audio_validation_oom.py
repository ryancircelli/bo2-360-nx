"""Fault-inject allocations in the production validation wrapper, using the real PPCContext.

Only this standalone fixture is compiled. No generated code, game assets or runtime is linked.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "games/bo2/src/bo2zm_native_audio.cpp")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")

    def extract(pattern):
        match = re.search(pattern, source, re.M | re.S)
        if not match:
            raise RuntimeError("production definition not found: " + pattern)
        return match.group()

    fragments = [extract(r"^using Function = .*?;"),
                 extract(r"^constexpr uint32_t kChecksForApply.*?^constexpr uint32_t kMaxBytesChecked.*?;"),
                 extract(r"^inline bool Contiguous\(.*?^}"),
                 extract(r"^struct Region \{.*?^};"),
                 extract(r"^struct Regions \{.*?^};"),
                 extract(r"^enum class Group .*?;"),
                 extract(r"^bool Enabled\(.*?^}"),
                 extract(r"^struct Replacement \{.*?^};")]
    for signature in ("inline float Load(", "void Save(uint8_t* base,", "void Restore(uint8_t* base,",
                      "bool Near(", "bool NearSkin(", "int Compare(", "bool CompareContext("):
        fragments.append(extract("^" + re.escape(signature) + r".*?^}"))
    fragments.append(extract(r"^thread_local int t_checking = .*?;"))
    # CompareContext does not use t_checking; Call does. The recovery helper is optional
    # so --source can also demonstrate that the fixture fails against the unpatched wrapper.
    if "void DisableSnapshotValidation(" in source:
        fragments.append(extract(r"^void DisableSnapshotValidation\(.*?^}"))
    fragments.append(extract(r"^template <typename FRegions>\nvoid Call\(.*?^}"))
    fixture = (ROOT / "tools/tests/audio_validation_oom_test.cpp").read_text(encoding="utf-8")
    program = fixture.replace("// PRODUCTION_VALIDATION_CODE", "\n\n".join(fragments))
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler and Path("C:/Program Files/LLVM/bin/clang++.exe").is_file():
        compiler = "C:/Program Files/LLVM/bin/clang++.exe"
    if not compiler:
        parser.error("set CXX to a C++23 compiler")
    includes = [ROOT / "sdk/include", ROOT / "sdk/thirdparty/simde",
                ROOT / "sdk/thirdparty/fmt/include", ROOT / "sdk/thirdparty/spdlog/include"]
    with tempfile.TemporaryDirectory(prefix="bo2-audio-validation-") as directory:
        cpp = Path(directory) / "audio_validation.cpp"
        exe = Path(directory) / ("audio_validation.exe" if os.name == "nt" else "audio_validation")
        cpp.write_text(program, encoding="utf-8")
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-msse4.1",
                        *["-I" + str(p) for p in includes], str(cpp), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
