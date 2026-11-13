"""Build the actual native cheat core with an authored guest-memory/VM boundary.

No game code, SDK build, real controllers, renderer or game process is used.
The fixture includes the complete production .cpp; no method is extracted or
rewritten. Platform context/imports, runtime services and guest calls are fakes.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "ppc.h": r'''#pragma once
#include <cstdint>
union FixtureRegister { uint64_t u64 = 0; uint32_t u32; int32_t s32; double f64; };
struct PPCContext {
  FixtureRegister r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13;
  FixtureRegister f0,f1,f2,f3,f4;
};
using PPCFunc = void(PPCContext&, uint8_t*);
#define REX_EXTERN(name) void name(PPCContext&, uint8_t*)
''',
    "ppc/func.h": "#pragma once\n#include <rex/ppc.h>\nnamespace rex::runtime { PPCFunc* ResolveIndirectFunction(uint32_t); }\n",
    "hook.h": "#pragma once\n#define REX_HOOK_RAW(name) void name(PPCContext& ctx, uint8_t* base)\n",
    "logging.h": r'''#pragma once
#include <fmt/format.h>
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
''',
    "cvar.h": r'''#pragma once
#include <string>
namespace rex::cvar {
enum class Lifecycle { kInitOnly };
struct Registrar { Registrar& lifecycle(Lifecycle) { return *this; } };
}
#define REXCVAR_DEFINE_STRING(name,value,...) inline std::string FLAGS_##name=value; \
  static auto reg_##name=rex::cvar::Registrar{}
#define REXCVAR_DEFINE_BOOL(name,value,...) inline bool FLAGS_##name=value; \
  static auto reg_##name=rex::cvar::Registrar{}
#define REXCVAR_GET(name) FLAGS_##name
#define REXCVAR_ALIAS(alias, name) static_assert(true)
''',
    "runtime.h": r'''#pragma once
#include <cstdint>
namespace rex {
struct FixtureHeap {
  bool Protect(uint32_t, uint32_t, uint32_t, uint32_t* = nullptr) { return false; }
  uint32_t page_size() { return 4096; }
  uint32_t GetUnreservedPageCount() { return 0; }
  uint32_t GetTotalPageCount() { return 0; }
};
struct FixtureMemory {
  FixtureHeap* LookupHeap(uint32_t) { return nullptr; }
  uint8_t* virtual_membase() { return nullptr; }
};
struct Runtime {
  static Runtime* instance() { return nullptr; }
  FixtureMemory* memory() { return nullptr; }
};
}
''',
    "system/xmemory.h": "#pragma once\nnamespace rex::memory { enum {kMemoryProtectRead=1,kMemoryProtectWrite=2}; }\n",
}


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="bo2-player-cheats-") as folder:
        temporary = Path(folder)
        includes = temporary / "include/rex"
        for name, source in STUBS.items():
            path = includes / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
        executable = temporary / "player_cheats.exe"
        subprocess.run([compiler, "-std=c++23", "-O1", "-g", "-pthread", "-DFMT_HEADER_ONLY",
                        "-I" + str(includes.parent), "-I" + str(ROOT / "sdk/thirdparty/fmt/include"),
                        str(ROOT / "tools/tests/player_cheats_test.cpp"), "-o", str(executable)],
                       check=True, env=env, cwd=ROOT)
        subprocess.run([str(executable)], check=True, env=env, cwd=ROOT, timeout=40)


if __name__ == "__main__":
    main()
