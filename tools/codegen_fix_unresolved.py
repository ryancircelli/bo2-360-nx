"""Iterate rexglue codegen analysis, declaring tail-branch targets as functions.

When the analyzer reports `b <target> ... target not in any function`, the
target is usually a small thunk or setter reached only by tail branches.
It is declared as a standalone function only when the instruction right
before it ends a function (blr, b, bctr, or zero padding). Anything else may
be the middle of a function and is left for a human, and the loop stops.

usage: python tools/codegen_fix_unresolved.py [--max-rounds N] [--project DIR --manifest FILE --image FILE]
Run from the repository root. Appends to <project>/overrides.toml (default games/bo2/, bo2zm_manifest.toml and the
default_mp image).
"""
import argparse
import os
import re
import struct
import subprocess
import sys

import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "games", "bo2")
IMAGE = os.path.join(ROOT, "work", "dump_tu18", "default_mp.image.bin")
# The host tools (tools/build_host.sh); a worktree without its own build can point REXGLUE_BIN at another checkout's.
SDK_BIN = os.environ.get("REXGLUE_BIN") or os.path.join(ROOT, "sdk", "out", "win-amd64")
OVERRIDES = os.path.join(APP, "overrides.toml")
LOG = os.path.join(APP, "codegen_analyze.log")
BASE = 0x82000000

UNRESOLVED = re.compile(r"^\s+0x([0-9A-F]{8}) from 0x([0-9A-F]{8}): (b\w*) 0x[0-9A-F]+ from .* target not in any function")

data = b""
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)


def word(ea):
    return struct.unpack(">I", data[ea - BASE: ea - BASE + 4])[0]


def dis(ea):
    ins = next(md.disasm(data[ea - BASE: ea - BASE + 4], ea), None)
    return f"{ins.mnemonic} {ins.op_str}".strip() if ins else f".long 0x{word(ea):08X}"


def ends_function(ea):
    w = word(ea)
    if w == 0:
        return True                      # padding
    if w == 0x4E800020 or w == 0x4E800420:
        return True                      # blr / bctr
    if (w >> 26) == 18 and (w & 1) == 0:
        return True                      # b / ba (not bl)
    return False


def summary(ea, limit=6):
    parts = []
    for i in range(limit):
        text = dis(ea + 4 * i)
        parts.append(text)
        if text.startswith(("blr", "bctr")) or (text.startswith("b ") and not text.startswith("bl")):
            break
    return "; ".join(parts)


def run_codegen(manifest):
    env = dict(os.environ, PATH=SDK_BIN + os.pathsep + os.environ["PATH"])
    with open(LOG, "w", encoding="utf-8", errors="replace") as log:
        rc = subprocess.call([os.path.join(SDK_BIN, "rexglue"), "--log-level", "debug",
                              "codegen", manifest], cwd=APP, stdout=log,
                             stderr=subprocess.STDOUT, env=env)
    text = open(LOG, encoding="utf-8", errors="replace").read()
    errors = [m.groups() for m in map(UNRESOLVED.match, text.splitlines()) if m]
    other = re.findall(r"^(\w+) \((\d+)\):", text, re.M)
    return rc, errors, other, text


def main():
    global APP, OVERRIDES, LOG, data
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-rounds", type=int, default=15)
    ap.add_argument("--project", default=APP, help="the codegen project folder (default games/bo2/)")
    ap.add_argument("--manifest", default="bo2zm_manifest.toml", help="the manifest, inside --project")
    ap.add_argument("--image", default=IMAGE, help="the decrypted image the manifest's XEX gives")
    a = ap.parse_args()
    APP = os.path.abspath(a.project)
    OVERRIDES = os.path.join(APP, "overrides.toml")
    LOG = os.path.join(APP, "codegen_analyze.log")
    data = open(a.image, "rb").read()
    sys.stdout.reconfigure(encoding="utf-8")

    for rnd in range(1, a.max_rounds + 1):
        rc, errors, other, text = run_codegen(a.manifest)
        stats = re.search(r"fixed point at iteration \d+ \((\d+) functions\)", text)
        print(f"round {rnd}: exit {rc}, {stats.group(1) if stats else '?'} functions, "
              f"{len(errors)} unresolved tail branches, error groups {other}")
        if not errors:
            return 0 if rc == 0 else 1
        known = open(OVERRIDES, encoding="utf-8").read()
        lines, manual = [], []
        for target, site, mnem in sorted(set(errors)):
            t = int(target, 16)
            if f'"0x{target}"' in known:
                manual.append((target, site, "already declared"))
                continue
            if mnem != "b" or not ends_function(t - 4):
                manual.append((target, site, f"before target: {dis(t - 4)}"))
                continue
            lines.append(f'"0x{target}" = {{ }}   # {summary(t)}   <- 0x{site}')
        if manual:
            print("needs a human:")
            for target, site, why in manual:
                print(f"  0x{target} <- 0x{site}: {why}")
            return 2
        with open(OVERRIDES, "a", encoding="utf-8") as f:
            f.write(f"\n# --- Tail-branch targets, round {rnd} (tools/codegen_fix_unresolved.py) ---\n")
            f.write("\n".join(lines) + "\n")
        for line in lines:
            print("  +", line)
    return 3


if __name__ == "__main__":
    sys.exit(main())
