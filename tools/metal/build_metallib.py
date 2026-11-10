#!/usr/bin/env python3
"""Compile the MSL that bo2zm_spv2msl wrote into Metal libraries, ahead of time.

    build_metallib.py <out_dir> [--jobs N] [--shards 16]

Reads <out_dir>/msl/*.metal, compiles each one to <out_dir>/air/<name>.air with the Xcode Metal toolchain
(xcrun metal; skipped when the .air is newer than the .metal), links them into <out_dir>/lib/bo2zm_<shard>.metallib
(shard = first hex digit(s) of the shader hash, so the runtime finds a shader's library from its name alone), and
writes <out_dir>/metallib.tsv (name, result, message) and <out_dir>/report.txt (counts and failure classes,
including the SPIR-V to MSL failures from spv2msl.tsv).

Everything under <out_dir> is derived from game data: keep it outside the repository.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

STD = "metal3.1"
MIN_MACOS = "14.0"


def compile_one(out_dir, name):
    src = os.path.join(out_dir, "msl", name + ".metal")
    air = os.path.join(out_dir, "air", name + ".air")
    if os.path.exists(air) and os.path.getmtime(air) >= os.path.getmtime(src):
        return name, "ok", ""
    r = subprocess.run(
        ["xcrun", "-sdk", "macosx", "metal", "-c", f"-std={STD}", f"-mmacosx-version-min={MIN_MACOS}", "-O2",
         "-Wno-unused-variable", "-o", air, src],
        capture_output=True, text=True)
    if r.returncode == 0:
        return name, "ok", ""
    with open(os.path.join(out_dir, "err", name + ".txt"), "w") as f:
        f.write(r.stderr)
    return name, "metal", first_error(r.stderr)


def first_error(stderr):
    for line in stderr.splitlines():
        if "error:" in line:
            return line.split("error:", 1)[1].strip()
    return stderr.strip().splitlines()[-1] if stderr.strip() else "unknown"


def failure_class(message):
    """The message without the parts that differ per shader (names, numbers), to group failures."""
    message = re.sub(r"'[^']*'", "'_'", message)
    message = re.sub(r"\b(vs|ps)_[0-9a-f]{16}(_ez)?\b", "<shader>", message)
    return re.sub(r"\d+", "N", message)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out_dir")
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("--shards", type=int, default=16, choices=[1, 16, 256])
    a = p.parse_args()
    out = a.out_dir
    for d in ("air", "err", "lib"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    names = sorted(f[:-6] for f in os.listdir(os.path.join(out, "msl")) if f.endswith(".metal"))

    results = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for i, (name, result, message) in enumerate(pool.map(lambda n: compile_one(out, n), names), 1):
            results[name] = (result, message)
            if i % 500 == 0:
                print(f"  {i}/{len(names)} compiled", file=sys.stderr)

    digits = {1: 0, 16: 1, 256: 2}[a.shards]
    shards = collections.defaultdict(list)
    for name, (result, _) in results.items():
        if result == "ok":
            shards[name[3:3 + digits] if digits else "all"].append(os.path.join(out, "air", name + ".air"))
    link_failures = 0
    for shard, airs in sorted(shards.items()):
        lib = os.path.join(out, "lib", f"bo2zm_{shard}.metallib")
        r = subprocess.run(["xcrun", "-sdk", "macosx", "metallib", "-o", lib] + airs, capture_output=True, text=True)
        if r.returncode != 0:
            link_failures += 1
            print(f"link {lib}: {first_error(r.stderr)}", file=sys.stderr)

    with open(os.path.join(out, "metallib.tsv"), "w") as f:
        for name in names:
            result, message = results[name]
            f.write(f"{name}\t{result}\t{message}\n")

    # The whole picture: SPIR-V to MSL (spv2msl.tsv) then MSL to AIR (here).
    lines = []
    cross = {}
    tsv = os.path.join(out, "spv2msl.tsv")
    if os.path.exists(tsv):
        for row in open(tsv):
            name, stage, result, message = (row.rstrip("\n").split("\t") + [""])[:4]
            cross[name] = (stage, result, message)
    total = len(cross) or len(names)
    msl_ok = sum(1 for v in cross.values() if v[1] == "ok") if cross else len(names)
    air_ok = sum(1 for v in results.values() if v[0] == "ok")
    base = [n for n in (cross or results) if not n.endswith("_ez")]
    base_ok = sum(1 for n in base if results.get(n, ("",))[0] == "ok")
    lines.append(f"functions (shaders plus early-Z variants): {total}")
    lines.append(f"SPIR-V to MSL: {msl_ok} of {total}")
    lines.append(f"MSL to AIR: {air_ok} of {total}")
    lines.append(f"pack shaders compiled (without variants): {base_ok} of {len(base)}")
    lines.append(f"libraries: {len(shards)} ({link_failures} failed to link)")
    classes = collections.Counter()
    for name, (stage, result, message) in cross.items():
        if result != "ok":
            classes[("spirv-cross", failure_class(message))] += 1
    for name, (result, message) in results.items():
        if result != "ok":
            classes[("metal", failure_class(message))] += 1
    if classes:
        lines.append("failure classes:")
        for (where, message), n in classes.most_common():
            lines.append(f"  {n:6d}  {where}: {message}")
    report = "\n".join(lines) + "\n"
    with open(os.path.join(out, "report.txt"), "w") as f:
        f.write(report)
    print(report, end="")
    return 0 if air_ok == total and not link_failures else 1


if __name__ == "__main__":
    sys.exit(main())
