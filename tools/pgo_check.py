#!/usr/bin/env python3
"""Does a PGO profile still apply to the code? (Switch, BO2_PGO=use)

  python tools/pgo_check.py [--build games/bo2/out/switch[-<tag>]] [--pgo DIR] [--file bo2zm_recomp.N.cpp] [--min 0.5]
  python tools/pgo_check.py --native [--build ...] [--pgo DIR] [--min 0.5]

GCC finds a function's counts in the profile by a hash of its name, but drops them, silently, when the function's
control flow no longer has the checksum it had in the training build: the warning (-Wcoverage-mismatch) points at
DEFINE_REX_FUNC, which comes from the precompiled header, a system header, and games/bo2/CMakeLists.txt also passes
-Wno-coverage-mismatch. So a profile trained before the generated code or its flags changed (plain guest memory, a
code generator fix) can stop applying to the hot functions without a word (once, a profile two days old gave counts
to 7 of the 35 trained functions of bo2zm_recomp.10.cpp and none to its hottest one; retrained, the build was 9 %
faster).

This compiles one generated file the way the build does (flags from <build>/build.ninja, without LTO), with the
profile and -fdump-ipa-profile, in a scratch folder below the build folder laid out like it (the profile's file names
come from the object's path), and counts the functions that read non-zero counts against the functions the profile
has non-zero counts for. By default the file is the one with the most trained functions. Exit 1 if the share is
below --min (default 0.5): retrain (BO2_PGO=generate, notes in games/bo2/build_switch.sh).

--native does the same for each of the app's own instrumented files the profile has (the renderer, culling, audio...;
the list in games/bo2/CMakeLists.txt), which change far more often than the generated code: it prints each file's share and
the share over all of them. A file that is no longer in the build, or has no counts, is skipped. A profile trained on
the very code it is checked against reads about 98 % there (908 of 922 functions in one check).

Each file is compiled under the source path its build.ninja gives: GCC mixes the file name into the profile id of
every function not visible outside its file (anonymous namespaces, static functions, lambdas: most of the renderer),
so another spelling of the path finds none of their counts (joining the path itself, this tool read 46-48 % for a
fitting profile). The same holds for real builds: a build from another checkout (a worktree) gets no
counts for those functions from a profile trained in the main checkout; train in the checkout that builds.
"""
import argparse
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DKA = os.environ.get("DEVKITPRO", "")
if not os.path.isdir(os.path.join(DKA, "devkitA64")):  # unset, or MSYS-style (/opt/devkitpro)
    DKA = "C:/devkitPro"
GCC = os.path.join(DKA, "devkitA64", "bin", "aarch64-none-elf-g++.exe")
DUMP = os.path.join(DKA, "devkitA64", "bin", "aarch64-none-elf-gcov-dump.exe")
OBJ_DIR = "CMakeFiles/bo2zm_recomp.dir/generated/default_mp"
GEN_PREFIX = "CMakeFiles#bo2zm_recomp.dir#generated#default_mp#"
APP_PREFIX = "CMakeFiles#bo2zm.dir#"


def trained(gcda):
    """Number of functions with non-zero arc counts in one profile file."""
    out = subprocess.run([DUMP, "-l", gcda], capture_output=True, text=True).stdout
    return sum(1 for line in out.splitlines() if "COUNTERS arcs" in line and "all zero" not in line)


def reading(build, pgo, obj):
    """Functions that read non-zero counts when <build>/<obj>'s source is compiled with the profile; None when the
    object is not in the build."""
    ninja = open(os.path.join(build, "build.ninja"), encoding="utf-8").read()
    m = re.search(r"^build " + re.escape(obj) + r": \S+ (.+?)(?: \|.*)?\n((?:  .*\n)+)", ninja, re.M)
    if not m:
        return None
    # The source exactly as the build spells it: GCC mixes the file name into the profile id of every function that
    # is not visible outside its file (anonymous namespaces, static functions, lambdas), so another spelling of the
    # same file (backslashes, another checkout) finds none of their counts.
    src = re.sub(r"\$(.)", r"\1", m.group(1))
    var = dict(re.findall(r"^  (\w+) = (.*)$", m.group(2), re.M))
    # POSIX splitting: CMake quotes inside a word (-I"C:/a b/c") when the checkout's path has spaces, and only POSIX
    # mode joins that; the backslashes ninja writes sit inside double quotes, where POSIX mode keeps them.
    flags = shlex.split(var.get("DEFINES", "") + " " + var.get("FLAGS", "") + " " + var.get("INCLUDES", ""))
    drop = ("-flto", "-ffat-lto-objects", "-fprofile-use", "-fprofile-prefix-path", "-fprofile-generate",
            "-Winvalid-pch", "-Wno-missing-profile")
    unquote = lambda f: f[1:-1] if len(f) > 1 and f[0] == f[-1] == '"' else f  # not -DX="Release"'s own quotes
    flags = [unquote(f) for f in flags if not unquote(f).startswith(drop)]

    scratch = os.path.join(build, "pgo_check")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(scratch, os.path.dirname(obj)))
    cmd = [GCC] + flags + [f"-fprofile-use={pgo}", f"-fprofile-prefix-path={os.path.normpath(scratch)}",
                           "-fprofile-partial-training", "-fprofile-correction", "-Wno-coverage-mismatch",
                           "-fdump-ipa-profile", "-c", src, "-o", obj]
    r = subprocess.run(cmd, cwd=scratch, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"pgo_check: the compile of {src} failed:\n" + r.stderr[-2000:])
    if "profile count data file not found" in r.stderr:
        sys.exit(f"pgo_check: GCC found no profile for {src} in {pgo}")
    dumps = glob.glob(os.path.join(scratch, os.path.dirname(obj), "*.profile"))
    text = open(dumps[0], encoding="utf-8", errors="replace").read() if dumps else ""
    n = 0
    for part in re.split(r"^;; Function ", text, flags=re.M)[1:]:
        if any(int(c) for c in re.findall(r"^Read edge from \d+ to \d+, count:(\d+)", part, re.M)):
            n += 1
    shutil.rmtree(scratch, ignore_errors=True)
    return n


def check_generated(a):
    if a.file:
        name = a.file
        n_trained = trained(os.path.join(a.pgo, GEN_PREFIX + name + ".gcda"))
    else:
        best = (-1, None)
        for g in glob.glob(os.path.join(a.pgo, GEN_PREFIX + "bo2zm_recomp.*.cpp.gcda")):
            best = max(best, (trained(g), os.path.basename(g)[len(GEN_PREFIX):-len(".gcda")]))
        n_trained, name = best
        if name is None:
            sys.exit(f"pgo_check: no profile files in {a.pgo}")
    n = reading(a.build, a.pgo, f"{OBJ_DIR}/{name}.obj")
    if n is None:
        sys.exit(f"pgo_check: {name} is not in {a.build}/build.ninja (configure the build first)")
    share = n / n_trained if n_trained else 0.0
    print(f"pgo_check: {name}: {n} functions read non-zero counts, of {n_trained} trained in {a.pgo} "
          f"({100 * share:.0f} %)")
    if share < a.min:
        print(f"pgo_check: the profile no longer matches the generated code: retrain it (BO2_PGO=generate)",
              file=sys.stderr)
        sys.exit(1)


def check_native(a):
    total_read = total_trained = 0
    for gcda in sorted(glob.glob(os.path.join(a.pgo, APP_PREFIX + "*.gcda"))):
        rel = os.path.basename(gcda)[len(APP_PREFIX):-len(".gcda")].replace("#", "/")  # src/native/...cpp, or __/__/core/...cpp (CMake spells ../ as __/)
        n_trained = trained(gcda)
        if n_trained == 0 or not os.path.isfile(os.path.join(ROOT, "games", "bo2", rel.replace("__/", "../"))):
            continue
        n = reading(a.build, a.pgo, f"CMakeFiles/bo2zm.dir/{rel}.obj")
        if n is None:
            print(f"pgo_check: {rel}: not in the build, skipped")
            continue
        total_read += min(n, n_trained)
        total_trained += n_trained
        print(f"pgo_check: {rel}: {n} functions read non-zero counts, of {n_trained} trained "
              f"({100 * n / n_trained:.0f} %)")
    share = total_read / total_trained if total_trained else 0.0
    print(f"pgo_check: the app's native code: {total_read} of {total_trained} trained functions read counts "
          f"({100 * share:.0f} %) in {a.pgo}")
    if share < a.min:
        print(f"pgo_check: the profile no longer matches the app's native code: retrain it (BO2_PGO=generate)",
              file=sys.stderr)
        sys.exit(1)


def data_dir():
    """The data folder: $BO2_DATA, else the path in bo2_data.txt at the repository's root, else <root>/data. Git Bash
    exports Windows paths as /e/..., which Windows Python cannot open, so those are turned back into E:/..."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    value = os.environ.get("BO2_DATA", "")
    if not value:
        try:
            with open(os.path.join(root, "bo2_data.txt"), encoding="utf-8") as f:
                value = f.read().strip()
        except OSError:
            value = ""
    value = value or os.path.join(root, "data")
    m = re.match(r"^/([a-zA-Z])/(.*)$", value)
    if m:
        value = f"{m.group(1).upper()}:/{m.group(2)}"
    return os.path.abspath(value)


def main():
    ap = argparse.ArgumentParser()
    tag = os.environ.get("BO2_BUILD_TAG")
    ap.add_argument("--build", default=os.path.join(ROOT, "games", "bo2", "out", "switch" + (f"-{tag}" if tag else "")))
    ap.add_argument("--pgo", default=os.path.join(data_dir(), "pgo"))
    ap.add_argument("--file", help="bo2zm_recomp.N.cpp (default: the one with the most trained functions)")
    ap.add_argument("--native", action="store_true", help="check the app's own instrumented files instead")
    ap.add_argument("--min", type=float, default=0.5, help="exit 1 below this share")
    a = ap.parse_args()
    a.build = os.path.abspath(a.build)
    if a.native:
        check_native(a)
    else:
        check_generated(a)


if __name__ == "__main__":
    main()
