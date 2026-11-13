# Sums the "[memcpy_callers]" lines of a BO2_MEMCPY_CALLERS test build's log (core/bo2zm_memcpy_callers.cpp) by
# call site and names each one with addr2line -i: the inlined function the call is in, and the outermost function's
# file:line (an ELF with line tables of the same commit: a BO2_DEBUG_LINES=1 build).
# usage: python tools/memcpy_callers_sym.py bo2zm_NNN.log bo2zm_<commit>_lines.elf [--top N]
import argparse, re, subprocess
from collections import defaultdict

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("elf")
ap.add_argument("--top", type=int, default=30)
a = ap.parse_args()
ADDR2LINE = r"C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe"

ITEM = re.compile(r" (memcpy|memmove|memset|memcmp) image\+0x([0-9a-f]+) x(\d+) ([\d.]+) KB")
calls = defaultdict(float)
kb = defaultdict(float)
reports = 0
for line in open(a.log, encoding="utf-8", errors="replace"):
    if "[memcpy_callers] last" not in line:
        continue
    reports += 1
    seen = set()
    for m in ITEM.finditer(line):
        key = (m.group(1), m.group(2))
        if key in seen:  # each site is listed by bytes and again by calls
            continue
        seen.add(key)
        calls[key] += int(m.group(3))
        kb[key] += float(m.group(4))

addrs = sorted({k[1] for k in calls})
chains = {}
for i in range(0, len(addrs), 500):
    part = addrs[i:i + 500]
    out = subprocess.run([ADDR2LINE, "-i", "-f", "-C", "-a", "-e", a.elf] + ["0x" + x for x in part],
                         capture_output=True, text=True).stdout.splitlines()
    cur, frames = None, []
    for l in out + ["0x0"]:
        if re.match(r"^0x[0-9a-f]+$", l):
            if cur is not None:
                chains[cur] = frames
            cur, frames = l[2:].lstrip("0"), []
            continue
        frames.append(l)


def clean(name):
    name = name.replace("(anonymous namespace)::", "").replace("bo2zm::native::", "")
    return re.sub(r"\(.*", "", name)[-44:]


def where(addr):
    fr = chains.get(addr.lstrip("0"), [])
    if len(fr) < 2:
        return "?"
    loc = lambda k: fr[k].replace("\\", "/").split("/")[-1].split(" ")[0]
    if len(fr) <= 2:
        return "%s@%s" % (clean(fr[0]), loc(1))
    return "%s@%s in %s@%s" % (clean(fr[0]), loc(1), clean(fr[-2]), loc(len(fr) - 1))


print("%d reports (10 s each); per second:" % reports)
for title, order in (("by bytes", kb), ("by calls", calls)):
    print("-- " + title)
    for key in sorted(order, key=lambda k: -order[k])[:a.top]:
        n = calls[key] / reports / 10.0
        mb = kb[key] / reports / 10.0 / 1024.0
        print("%9.0f calls/s %7.2f MB/s %5.0f B  %-7s %s" % (n, mb, kb[key] * 1024.0 / calls[key] if calls[key] else 0,
                                                            key[0], where(key[1])))
