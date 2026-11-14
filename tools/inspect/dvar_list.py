#!/usr/bin/env python3
"""Lists the dvars the game registers, with their flags, from the generated code's disassembly comments.

Each Dvar_Register call is found by its "bl" and the arguments are rebuilt from the instructions before it (lis,
addi, li, ori, mr in the same function). The register functions and where their arguments go (each argument takes
the next GPR slot, so float arguments skip one):
  bool   0x82498550 (name r3, default r4, flags r5, description r6)
  int    0x824985D0 (name r3, default r4, min r5, max r6, flags r7, description r8)
  float  0x82498668 (name r3, default f1, min f2, max f3, flags r7, description r8)
  string 0x82498808 (name r3, default r4, flags r5, description r6)
  enum   0x82498880 (name r3, values r4, default r5, flags r6, description r7)
Flag 0x80 is cheat-protected (sv_cheats must be on to change it from the console). The default, min and max are shown
where they are immediates or float constants (lfs from the image).

usage: python -I tools/inspect/dvar_list.py [--cheat] [--grep TEXT]
The output names game dvars and quotes their descriptions: game-derived, keep it out of the repo.
"""
import os
import re
import struct
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GEN = os.path.join(REPO, "games", "bo2", "generated", "default_mp")
IMAGE = os.path.join(REPO, "work", "dump_tu18", "default_mp.image.bin")
BASE = 0x82000000
KINDS = {0x82498550: ("bool", 5, 6), 0x824985D0: ("int", 7, 8), 0x82498668: ("float", 7, 8),
         0x82498808: ("string", 5, 6), 0x82498880: ("enum", 6, 7)}
INSN = re.compile(r"^\s*// (\w+\.?) ?(.*)$")


def string_at(image, address):
    if address is None or not BASE <= address < BASE + len(image):
        return None
    end = image.find(b"\0", address - BASE)
    text = image[address - BASE:end]
    return text.decode("latin1") if 0 < len(text) < 300 and all(32 <= b < 127 for b in text) else None


def float_at(image, address):
    if address is None or not BASE <= address < BASE + len(image) - 4:
        return None
    return struct.unpack_from(">f", image, address - BASE)[0]


def number(v):
    if v is None:
        return "?"
    if isinstance(v, float):
        return "%g" % v
    return str(v - (1 << 32) if v & 0x80000000 else v)


def scan(image):
    for name in sorted(os.listdir(GEN)):
        if not name.endswith(".cpp"):
            continue
        regs = {}
        function = None
        for line in open(os.path.join(GEN, name), encoding="utf-8", errors="replace"):
            if line.startswith("DEFINE_REX_FUNC("):
                function = line[16:line.index(")")]
                regs = {}
                continue
            m = INSN.match(line)
            if not m:
                continue
            op, args = m.group(1), [a.strip() for a in m.group(2).split(",")]
            try:
                if op == "lfs" and re.fullmatch(r"(-?\d+)\((r\d+)\)", args[1]):
                    off, base_reg = re.fullmatch(r"(-?\d+)\((r\d+)\)", args[1]).groups()
                    if base_reg in regs:
                        regs[args[0]] = float_at(image, (regs[base_reg] + int(off)) & 0xFFFFFFFF)
                    else:
                        regs.pop(args[0], None)
                elif op == "lis":
                    v = int(args[1], 0)
                    regs[args[0]] = ((v & 0xFFFF) << 16) & 0xFFFFFFFF
                elif op == "li":
                    regs[args[0]] = int(args[1], 0) & 0xFFFFFFFF
                elif op == "addi" and args[1] in regs:
                    regs[args[0]] = (regs[args[1]] + int(args[2], 0)) & 0xFFFFFFFF
                elif op == "ori" and args[1] in regs:
                    regs[args[0]] = regs[args[1]] | int(args[2], 0)
                elif op == "mr":
                    if args[1] in regs:
                        regs[args[0]] = regs[args[1]]
                    else:
                        regs.pop(args[0], None)
                elif op == "bl":
                    target = int(args[0], 0)
                    if target in KINDS:
                        kind, flags_reg, desc_reg = KINDS[target]
                        dvar = string_at(image, regs.get("r3"))
                        if dvar:
                            if kind == "float":
                                limits = (regs.get("f1"), regs.get("f2"), regs.get("f3"))
                            elif kind == "int":
                                limits = (regs.get("r4"), regs.get("r5"), regs.get("r6"))
                            elif kind == "bool":
                                limits = (regs.get("r4"), None, None)
                            else:
                                limits = (None, None, None)
                            yield (dvar, kind, regs.get("r%d" % flags_reg), string_at(image, regs.get("r%d" % desc_reg)),
                                   function, limits)
                    for r in ("r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "f1", "f2", "f3"):
                        regs.pop(r, None)
                elif args and re.fullmatch(r"r\d+", args[0]):
                    regs.pop(args[0], None)
            except (ValueError, IndexError):
                regs.pop(args[0] if args else None, None)


def main():
    image = open(IMAGE, "rb").read()
    cheat_only = "--cheat" in sys.argv
    grep = sys.argv[sys.argv.index("--grep") + 1].lower() if "--grep" in sys.argv else None
    seen = set()
    for dvar, kind, flags, desc, function, limits in scan(image):
        if (dvar, kind) in seen:
            continue
        seen.add((dvar, kind))
        if cheat_only and (flags is None or not flags & 0x80):
            continue
        if grep and grep not in dvar.lower() and grep not in (desc or "").lower():
            continue
        flag_text = "%5X" % flags if flags is not None else "    ?"
        values = "%s [%s..%s]" % tuple(number(v) for v in limits) if kind in ("int", "float") else             (number(limits[0]) if kind == "bool" else "")
        print("%-36s %-6s %s  %-18s %-12s %s" % (dvar, kind, flag_text, values, function, desc or ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
