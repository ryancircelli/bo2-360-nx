"""Index the generated C++ (games/bo2/generated/default_mp) by function.

The recompiler keeps each PowerPC instruction as a comment ("// bl 0x82161700"),
so the call graph and the constants each function builds can be read back
without Ghidra. Builds a JSON index once, then answers queries:

  python tools/inspect/gen_index.py build
  python tools/inspect/gen_index.py fn 0x821715A8          # calls (incl. tail calls), callers, imports, imm, size
  python tools/inspect/gen_index.py callers 0x82161700
  python tools/inspect/gen_index.py range 0x82160000 0x82190000   # functions in a range, sorted
  python tools/inspect/gen_index.py imm 8704 [lo hi]        # functions using an immediate

Index: work/gen_index.json (git-ignored: derived from game code).
"""

import glob
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
GEN = os.path.join(ROOT, "games", "bo2", "generated", "default_mp")
INDEX = os.path.join(ROOT, "work", "gen_index.json")

RE_FN = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
RE_INSN = re.compile(r"^\s*// (\w[\w.]*) ?(.*)$")
RE_IMPORT = re.compile(r"__imp__([A-Za-z_][A-Za-z0-9_]*)\(ctx, base\)")


def build():
    fns = {}
    for path in sorted(glob.glob(os.path.join(GEN, "bo2zm_recomp.*.cpp"))):
        cur = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = RE_FN.match(line)
                if m:
                    cur = {"calls": [], "tail": [], "imports": [], "imm": [], "insns": 0}
                    fns[m.group(1)] = cur
                    continue
                if cur is None:
                    continue
                m = RE_INSN.match(line)
                if m:
                    op, args = m.group(1), m.group(2)
                    cur["insns"] += 1
                    if op in ("bl",) and args.startswith("0x"):
                        cur["calls"].append(args[2:].upper())
                    elif op == "b" and args.startswith("0x"):
                        cur["tail"].append(args[2:].upper())
                    elif op in ("li", "lis", "ori", "oris", "addi", "addis", "cmpwi", "cmplwi"):
                        tail = args.rsplit(",", 1)[-1]
                        if re.fullmatch(r"-?\d+", tail):
                            cur["imm"].append(f"{op} {tail}")
                    continue
                m = RE_IMPORT.search(line)
                if m and "DECLARE" not in line:
                    cur["imports"].append(m.group(1))
    callers = {}
    for name, info in fns.items():
        # A plain `b` to the start of another function is a tail call (e.g. sub_828B7540 ends in
        # `b 0x82169bf8`); count it as a call, or those callers are missing. `b` inside the function
        # or to the save/restore helpers' middles doesn't start a function and is dropped.
        info["tail"] = sorted({t for t in info["tail"] if t in fns and t != name})
        info["calls"] = sorted(set(info["calls"]) | set(info["tail"]))
        info["imports"] = sorted(set(info["imports"]))
        for c in info["calls"]:
            callers.setdefault(c, []).append(name)
    for name, info in fns.items():
        info["callers"] = sorted(callers.get(name, []))
    os.makedirs(os.path.dirname(INDEX), exist_ok=True)
    with open(INDEX, "w") as f:
        json.dump(fns, f)
    print(f"{len(fns)} functions -> {INDEX}")


def load():
    with open(INDEX) as f:
        return json.load(f)


def key(a):
    return f"{int(a, 16):08X}"


def show(fns, name):
    i = fns.get(name)
    if not i:
        print(f"sub_{name}: not a function start")
        return
    print(f"sub_{name}: {i['insns']} insns")
    print("  calls:  ", " ".join(i["calls"]))
    if i.get("tail"):
        print("  tail:   ", " ".join(i["tail"]))
    print("  callers:", " ".join(i["callers"][:40]), "..." if len(i["callers"]) > 40 else "")
    if i["imports"]:
        print("  imports:", " ".join(i["imports"]))
    print("  imm:    ", " | ".join(i["imm"][:60]))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    if cmd == "build":
        build()
        return
    if cmd == "help":
        print(__doc__)
        return
    fns = load()
    if cmd == "fn":
        for a in sys.argv[2:]:
            show(fns, key(a))
    elif cmd == "callers":
        print(" ".join(fns.get(key(sys.argv[2]), {}).get("callers", [])))
    elif cmd == "range":
        lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
        for n in sorted(fns):
            if lo <= int(n, 16) < hi:
                i = fns[n]
                print(f"{n} {i['insns']:5d} insns  calls {len(i['calls']):3d}  callers {len(i['callers']):4d}"
                      f"  {' '.join(i['imports'])}")
    elif cmd == "imm":
        v = sys.argv[2]
        lo, hi = (int(sys.argv[3], 16), int(sys.argv[4], 16)) if len(sys.argv) > 4 else (0, 1 << 32)
        for n in sorted(fns):
            if lo <= int(n, 16) < hi:
                hits = [x for x in fns[n]["imm"] if x.split()[-1] == v]
                if hits:
                    print(n, ", ".join(hits))


if __name__ == "__main__":
    main()
