"""Cross-references from the generated code's disassembly comments.

The recompiled C++ (games/bo2/generated/default_mp/*.cpp) keeps every PowerPC instruction as a comment inside
DEFINE_REX_FUNC(sub_XXXXXXXX). This finds which functions build a given address (lis + addi/ori/lwz/stw...), or
call a given function (bl), without a disassembler database. Read-only; the generated code is game-derived.

usage: python -I tools/inspect/genxref.py <hex address> [...] [--calls] [--context N]
  --calls     also list "bl <address>" call sites (the default when the address is a function start)
  --context N print N instruction comments around each hit
"""
import argparse
import glob
import os
import re

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GEN = os.path.join(REPO, "games", "bo2", "generated", "default_mp")
FUNC = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)")
INSN = re.compile(r"^\s*// (\w+\.?) ?(.*)$")
REG = r"r(\d+)"


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addresses", nargs="+")
    ap.add_argument("--calls", action="store_true")
    ap.add_argument("--context", type=int, default=0)
    a = ap.parse_args()
    targets = {int(x, 16) for x in a.addresses}
    hits = []
    for path in sorted(glob.glob(os.path.join(GEN, "*_recomp.*.cpp"))):
        func = None
        his = {}
        insns = []
        for line in open(path, encoding="utf-8", errors="replace"):
            m = FUNC.match(line)
            if m:
                func, his, insns = m.group(1), {}, []
                continue
            m = INSN.match(line)
            if not m or func is None:
                continue
            op, args = m.group(1), m.group(2).replace(" ", "")
            insns.append(f"{op} {args}")
            parts = args.split(",")
            if op == "lis" and len(parts) == 2:
                his[parts[0]] = (int(parts[1]) & 0xFFFF) << 16
                continue
            if op in ("bl", "b") and (a.calls or True):
                try:
                    t = int(parts[0], 16)
                except ValueError:
                    t = None
                if t in targets and (a.calls or op == "bl"):
                    hits.append((func, f"{op} {args}", list(insns)))
                continue
            # rD,rA,imm (addi/addic/ori) or rD,imm(rA) (loads/stores)
            m2 = re.match(r"(r\d+),(r\d+),(-?\d+)$", args)
            m3 = re.match(r"(r\d+|f\d+),(-?\d+)\((r\d+)\)$", args)
            base = imm = None
            if m2 and op in ("addi", "addic", "ori"):
                base, imm = m2.group(2), int(m2.group(3))
                if op == "ori":
                    imm &= 0xFFFF
                else:
                    imm = s16(imm & 0xFFFF)
            elif m3:
                base, imm = m3.group(3), s16(int(m3.group(2)) & 0xFFFF)
            if base in his and imm is not None:
                addr = (his[base] + imm) & 0xFFFFFFFF
                if addr in targets:
                    hits.append((func, f"{op} {args}", list(insns)))
    for func, insn, ctx in hits:
        print(f"{func}: {insn}")
        if a.context:
            for x in ctx[-a.context:]:
                print(f"    {x}")
    if not hits:
        print("no references found")


if __name__ == "__main__":
    main()
