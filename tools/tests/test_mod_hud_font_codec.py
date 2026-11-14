"""Compile read-only generated codec controls in a temporary authored fixture.

No generated function or game data is committed. No game process is started.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = ("82425B50", "82424750", "8241A178", "8241A5F0", "8241A0C8", "829160B8", "8268EDC0")

def main():
    bodies = {}
    for path in (ROOT/"games/bo2/generated/default_mp").glob("bo2zm_recomp.*.cpp"):
        source = path.read_text(encoding="utf-8")
        for function in FUNCTIONS:
            match = re.search(r"DEFINE_REX_FUNC\(sub_"+function+r"\) \{.*?(?=\nDEFINE_REX_FUNC|\Z)",source,re.S)
            if match:
                bodies[function] = match.group()
    if len(bodies) != len(FUNCTIONS):
        raise SystemExit("Existing generated codec bodies are required")
    controls = "\n".join(bodies[f] for f in FUNCTIONS)
    called = set(re.findall(r"__imp__sub_([0-9A-F]+)\(ctx, base\)", controls))
    declarations = "\n".join(f"void __imp__sub_{f}(PPCContext&,uint8_t*);" for f in sorted(called|set(FUNCTIONS)))
    stubs = "\n".join(f"void __imp__sub_{f}(PPCContext&,uint8_t*) {{ assert(false); }}" for f in sorted(called-set(FUNCTIONS)))
    fixture = (ROOT/"tools/tests/mod_hud_font_codec_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("// GENERATED_CONTROLS",declarations+"\n"+stubs+"\n"+controls)
    compiler = os.environ.get("CXX") or shutil.which("clang++") or "C:/Program Files/LLVM/bin/clang++.exe"
    with tempfile.TemporaryDirectory(prefix="bo2-hud-font-wire-") as temp:
        source = Path(temp)/"codec.cpp"; exe = Path(temp)/"codec.exe"
        source.write_text(fixture,encoding="utf-8")
        subprocess.run([compiler,"-std=c++23","-O2","-msse4.1","-ffp-contract=off",
                        "-I"+str(ROOT/"sdk/include"),"-I"+str(ROOT/"sdk/thirdparty/simde"),
                        str(source),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=20)

if __name__ == "__main__":
    main()
