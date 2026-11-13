"""Compiler barriers at the generated code's labels and ordering instructions (see core/bo2zm_guest_memory.h).

With the CMake option BO2ZM_GUEST_MEMORY_PLAIN the generated code's guest loads and stores are not volatile. What
kept a guest spin loop rereading its flag, and stores in order across a sync, is then this barrier: a line
`REX_GUEST_BARRIER();` right after every label (`loc_XXXXXXXX:`, every branch target, so every loop's back edge
lands on one) and after every sync, lwsync, eieio, isync and db16cyc (which the code generator writes as a comment
only). Without the option the macro is empty and the code is the same as without these lines.

The rewrite only adds or removes those lines, in bo2zm_recomp.*.cpp, and is idempotent. Run it after every codegen
(tools/codegen.sh does) and after direct_calls.py / localize.py in any order: neither looks at these lines.

usage: python tools/guest_barriers.py [--gen DIR] [--undo | --check]
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "app", "generated", "default_mp")
BARRIER = "\tREX_GUEST_BARRIER();"
LABEL = re.compile(r"^loc_[0-9A-F]+:\s*$")
ORDERING = re.compile(r"^\s*// (sync|lwsync|eieio|isync|db16cyc)\b")


def recomp_files(gen):
    return sorted(os.path.join(gen, f) for f in os.listdir(gen)
                  if re.fullmatch(r"\w+_recomp\.\d+\.cpp", f))


def rewrite(path, undo):
    lines = open(path, encoding="utf-8").read().split("\n")
    out = []
    labels = orderings = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        if line == BARRIER:  # ours: dropped here, put back below where it belongs
            i += 1
            continue
        out.append(line)
        if not undo:
            if LABEL.match(line):
                out.append(BARRIER)
                labels += 1
            elif ORDERING.match(line):
                out.append(BARRIER)
                orderings += 1
        i += 1
    return "\n".join(out), labels, orderings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=GEN)
    ap.add_argument("--undo", action="store_true", help="remove every barrier line")
    ap.add_argument("--check", action="store_true", help="change nothing; fail if a file is not as this tool leaves it")
    a = ap.parse_args()
    labels = orderings = changed = 0
    stale = []
    for path in recomp_files(a.gen):
        text = open(path, encoding="utf-8").read()
        new, n_labels, n_orderings = rewrite(path, a.undo)
        labels += n_labels
        orderings += n_orderings
        if new != text:
            if a.check:
                stale.append(os.path.basename(path))
            else:
                open(path, "w", encoding="utf-8", newline="\n").write(new)
                changed += 1
    if a.check:
        if stale:
            print(f"guest_barriers --check: {len(stale)} files without their barriers (first: {stale[0]}); "
                  f"fix: python tools/guest_barriers.py --gen {a.gen}", file=sys.stderr)
            sys.exit(1)
        print(f"guest_barriers --check: ok ({labels} labels, {orderings} ordering instructions)")
        return
    verb = "removed" if a.undo else "placed"
    print(f"guest_barriers: {verb}: {labels} after labels, {orderings} after ordering instructions; "
          f"{changed} files changed")


if __name__ == "__main__":
    main()
