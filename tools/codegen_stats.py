"""Counts what makes the generated C++ slow, so codegen changes can be compared before/after.

usage: python tools/codegen_stats.py [generated folder]   (default: games/bo2/generated/default_mp)

Counts, over bo2zm_recomp.*.cpp:
  - calls to the register save/restore helpers (__savegprlr_N, __restgprlr_N, __savefpr_N, ...);
  - uses of ctx.xer, ctx.ctr, ctx.cr0-7 and of the non-volatile registers r14-r31 / f14-f31 kept in ctx;
  - calls between game functions: through the weak name (sub_X(ctx, base);) and direct (__imp__sub_X(ctx, base););
  - indirect calls (REX_CALL_INDIRECT_FUNC) and functions (DEFINE_REX_FUNC);
  - files, lines and bytes.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "games", "bo2", "generated", "default_mp")

PATTERNS = {
    "helper calls (__save*/__rest*)": re.compile(r"\b__(?:save|rest)(?:gprlr|fpr|vmx)_\d+\(ctx"),
    "  __savegprlr/__restgprlr": re.compile(r"\b__(?:save|rest)gprlr_\d+\(ctx"),
    "ctx.xer": re.compile(r"\bctx\.xer\b"),
    "ctx.ctr": re.compile(r"\bctx\.ctr\b"),
    "ctx.cr0-7": re.compile(r"\bctx\.cr[0-7]\b"),
    "ctx.r14-r31/f14-f31": re.compile(r"\bctx\.(?:r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01]))\b"),
    "calls sub_X(ctx, base)": re.compile(r"(?<![\w])sub_[0-9A-F]{8}\(ctx, base\);"),
    "calls __imp__sub_X(ctx, base)": re.compile(r"\b__imp__sub_[0-9A-F]{8}\(ctx, base\);"),
    "indirect calls": re.compile(r"\bREX_CALL_INDIRECT_FUNC\("),
    "functions": re.compile(r"^DEFINE_REX_FUNC\(", re.M),
}


def main():
    files = sorted(glob.glob(os.path.join(GEN, "*_recomp.*.cpp")))
    if not files:
        sys.exit(f"no *_recomp.*.cpp in {GEN}")
    totals = dict.fromkeys(PATTERNS, 0)
    lines = size = 0
    for f in files:
        text = open(f, encoding="utf-8", errors="ignore").read()
        size += os.path.getsize(f)
        lines += text.count("\n")
        for name, pat in PATTERNS.items():
            totals[name] += len(pat.findall(text))
    for name, n in totals.items():
        print(f"{name:34s} {n:>10,}")
    print(f"{'files':34s} {len(files):>10,}")
    print(f"{'lines':34s} {lines:>10,}")
    print(f"{'bytes':34s} {size:>10,}")


if __name__ == "__main__":
    main()
