"""Exercise the production HUD hooks and the CRT callback with authored guest memory.

The fixtures include the production sources directly and stub the rex headers. Two
pieces of generated code are taken from the local generated tree at test time and
written only to a temporary directory: the guest setjmp/longjmp transport from the
precompiled header, and the game's two screen placement lookups (sub_822A8570 and
sub_822A85A0), which serve as the stock functions the placement hooks call. Nothing
generated is kept or committed.

--source points the HUD fixture at another copy of bo2zm_mod_hud_fit.cpp, for
example a deliberately broken one, to confirm that the checks catch a regression.
"""
import argparse
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "ppc.h": '#pragma once\n#include <rex/ppc/context.h>\n',
    "hook.h": '#pragma once\n#include <rex/ppc/func.h>\n#define REX_HOOK_RAW(name) REX_FUNC(name)\n',
    "logging.h": '#pragma once\n#define REXLOG_INFO(...) FixtureLog(fmt::format(__VA_ARGS__))\n#define REXLOG_WARN(...) FixtureLog(fmt::format(__VA_ARGS__))\n#define REXLOG_ERROR(...) FixtureLog(fmt::format(__VA_ARGS__))\n',
    "cvar.h": '''#pragma once
namespace rex::cvar {
enum class Lifecycle {kInitOnly};
struct Registrar {
  Registrar& lifecycle(Lifecycle) { return *this; }
  Registrar& range(double, double) { return *this; }
};
}
#define REXCVAR_DEFINE_BOOL(name, value, ...) bool FLAGS_##name=value; static auto reg_##name=rex::cvar::Registrar{}
#define REXCVAR_DEFINE_DOUBLE(name, value, ...) double FLAGS_##name=value; static auto reg_##name=rex::cvar::Registrar{}
#define REXCVAR_GET(name) FLAGS_##name
#define REXCVAR_ALIAS(alias, name) static_assert(true)
''',
    "system/xmemory.h": '''#pragma once
#include <cstdint>
namespace rex::memory {
constexpr uint32_t kMemoryProtectRead = 1;
constexpr uint32_t kMemoryProtectWrite = 2;
template<class T> T GuestPtr(uint8_t* base, uint32_t at) { return reinterpret_cast<T>(base+at); }
}
''',
    "runtime.h": '''#pragma once
#include <cstdint>
#ifndef FIXTURE_QUERY_OBSERVER
#define FIXTURE_QUERY_OBSERVER(at) ((void)0)
#endif
#ifndef FIXTURE_QUERY_DENY
#define FIXTURE_QUERY_DENY(at) false
#endif
// Page protection of a guest address: read-only script code, read/write guest memory.
#ifndef FIXTURE_PROTECTION
#define FIXTURE_PROTECTION(at) ((at) >= 0x10000 && (at) < 0x14000 ? 1u : (at) >= 0x40000000 && (at) < 0xA0000000 ? 3u : 0u)
#endif
#ifndef FIXTURE_SYSTEM_HEAP_ALLOC
#define FIXTURE_SYSTEM_HEAP_ALLOC(size, alignment) 0x80000u
#endif
namespace rex {
struct FixtureHeap {
  bool QueryProtect(uint32_t at, uint32_t* protection) {
    FIXTURE_QUERY_OBSERVER(at);
    if (FIXTURE_QUERY_DENY(at)) { *protection = 0; return false; }
    *protection = FIXTURE_PROTECTION(at);
    return *protection != 0;
  }
};
struct FixtureMemory {
  FixtureHeap* LookupHeap(uint32_t) { static FixtureHeap h; return &h; }
  uint32_t SystemHeapAlloc([[maybe_unused]] uint32_t size, [[maybe_unused]] uint32_t alignment = 0x20, uint32_t = 0) {
    return FIXTURE_SYSTEM_HEAP_ALLOC(size, alignment);
  }
};
struct Runtime {
  static Runtime* instance() { static Runtime r; return &r; }
  FixtureMemory* memory() { static FixtureMemory m; return &m; }
};
}
''',
}
GENERATED_FUNCTIONS = ("sub_822A8570", "sub_822A85A0")


def generated_functions(directory):
    """The text of each needed generated function, read from the local generated tree."""
    patterns = {name: re.compile(r"DEFINE_REX_FUNC\(" + name + r"\) \{.*?\n\}\n", re.S)
                for name in GENERATED_FUNCTIONS}
    found = {}
    for path in sorted(directory.glob("bo2zm_recomp.*.cpp")):
        text = path.read_text(encoding="utf-8")
        for name, pattern in patterns.items():
            if name not in found:
                match = pattern.search(text)
                if match:
                    found[name] = match.group()
        if len(found) == len(patterns):
            return [found[name] for name in GENERATED_FUNCTIONS]
    missing = ", ".join(name for name in GENERATED_FUNCTIONS if name not in found)
    raise SystemExit(f"The generated tree must contain {missing}; run the code generator first")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=ROOT/"games/bo2/src/bo2zm_mod_hud_fit.cpp",
                        help="bo2zm_mod_hud_fit.cpp to test (default: the one in games/bo2/src)")
    arguments = parser.parse_args()
    source = arguments.source.resolve()
    compiler = os.environ.get("CXX") or shutil.which("clang++") or "C:/Program Files/LLVM/bin/clang++.exe"
    generated = ROOT/"app/generated/default_mp"
    with tempfile.TemporaryDirectory(prefix="bo2-mod-hud-") as temp:
        directory = Path(temp)
        (directory/"include").mkdir(parents=True)
        pch = (generated/"bo2zm_pch.h").read_text(encoding="utf-8")
        start = pch.index("inline std::unordered_map<u32, jmp_buf>& get_jmp_buf_map()")
        end = pch.index("//=============================================================================", start)
        jump = '#pragma once\n#include <csetjmp>\n#include <cstdlib>\n#include <unordered_map>\nusing u32=uint32_t;\n' + pch[start:end]
        (directory/"include/fixture_guest_jump.h").write_text(jump, encoding="utf-8")
        (directory/"include/fixture_placement_lookups.h").write_text("\n".join(generated_functions(generated)),
                                                                     encoding="utf-8")
        (directory/"include/fixture_mod_hud_fit_source.h").write_text(
            '#include "' + source.as_posix() + '"\n', encoding="utf-8")
        for name, text in STUBS.items():
            target = directory / "include/rex" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
        common = [compiler, "-std=c++23", "-O2", "-pthread", "-msse4.1", "-DFMT_HEADER_ONLY", "-D_CRT_SECURE_NO_WARNINGS",
                  "-Wall", "-Wextra", "-Werror", "-I"+str(directory/"include"), "-I"+str(ROOT/"games/bo2/src"), "-I"+str(ROOT/"core"),
                  "-I"+str(ROOT/"sdk/include"), "-I"+str(ROOT/"sdk/thirdparty/simde"),
                  "-I"+str(ROOT/"sdk/thirdparty/fmt/include")]
        for name in ("mod_hud_fit", "mod_hud_wire", "mod_hud_loader", "mod_hud_campaign_crt"):
            exe = directory / (name+".exe")
            linker = ["-Wl,/OPT:REF"] if os.name == "nt" else ["-Wl,--gc-sections"]
            subprocess.run([*common, "-ffunction-sections", "-fdata-sections", *linker,
                            str(ROOT/("tools/tests/"+name+"_test.cpp")), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)

if __name__ == "__main__":
    main()
